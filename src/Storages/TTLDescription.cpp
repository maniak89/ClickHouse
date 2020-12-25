#include <Storages/TTLDescription.h>

#include <Functions/IFunction.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTTTLElement.h>
#include <Parsers/ASTIdentifier.h>
#include <Storages/ColumnsDescription.h>
#include <Interpreters/Context.h>

#include <Parsers/queryToString.h>


#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>


namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int BAD_TTL_EXPRESSION;
}


TTLAggregateDescription::TTLAggregateDescription(const TTLAggregateDescription & other)
    : column_name(other.column_name)
    , expression_result_column_name(other.expression_result_column_name)
{
    if (other.expression)
        expression = other.expression->clone();
}

TTLAggregateDescription & TTLAggregateDescription::operator=(const TTLAggregateDescription & other)
{
    if (&other == this)
        return *this;

    column_name = other.column_name;
    expression_result_column_name = other.expression_result_column_name;
    if (other.expression)
        expression = other.expression->clone();
    else
        expression.reset();
    return *this;
}

namespace
{

void checkTTLExpression(const ExpressionActionsPtr & ttl_expression, const String & result_column_name)
{
    for (const auto & action : ttl_expression->getActions())
    {
        if (action.node->type == ActionsDAG::ActionType::FUNCTION)
        {
            IFunctionBase & func = *action.node->function_base;
            if (!func.isDeterministic())
                throw Exception(
                    "TTL expression cannot contain non-deterministic functions, "
                    "but contains function "
                        + func.getName(),
                    ErrorCodes::BAD_ARGUMENTS);
        }
    }

    const auto & result_column = ttl_expression->getSampleBlock().getByName(result_column_name);

    if (!typeid_cast<const DataTypeDateTime *>(result_column.type.get())
        && !typeid_cast<const DataTypeDate *>(result_column.type.get()))
    {
        throw Exception(
            "TTL expression result column should have DateTime or Date type, but has " + result_column.type->getName(),
            ErrorCodes::BAD_TTL_EXPRESSION);
    }
}

}

TTLDescription::TTLDescription(const TTLDescription & other)
    : mode(other.mode)
    , expression_ast(other.expression_ast ? other.expression_ast->clone() : nullptr)
    , result_column(other.result_column)
    , where_result_column(other.where_result_column)
    , group_by_keys(other.group_by_keys)
    , set_parts(other.set_parts)
    , aggregate_descriptions(other.aggregate_descriptions)
    , destination_type(other.destination_type)
    , destination_name(other.destination_name)
    , recompression_codec(other.recompression_codec)
{
    if (other.expression)
        expression = other.expression->clone();

    if (other.where_expression)
        where_expression = other.where_expression->clone();
}

TTLDescription & TTLDescription::operator=(const TTLDescription & other)
{
    if (&other == this)
        return *this;

    mode = other.mode;
    if (other.expression_ast)
        expression_ast = other.expression_ast->clone();
    else
        expression_ast.reset();

    if (other.expression)
        expression = other.expression->clone();
    else
        expression.reset();

    result_column = other.result_column;
    if (other.where_expression)
        where_expression = other.where_expression->clone();
    else
        where_expression.reset();

    where_result_column = other.where_result_column;
    group_by_keys = other.group_by_keys;
    set_parts = other.set_parts;
    aggregate_descriptions = other.aggregate_descriptions;
    destination_type = other.destination_type;
    destination_name = other.destination_name;

    if (other.recompression_codec)
        recompression_codec = other.recompression_codec->clone();
    else
        recompression_codec.reset();

    return * this;
}

TTLDescription TTLDescription::getTTLFromAST(
    const ASTPtr & definition_ast,
    const ColumnsDescription & columns,
    const Context & context,
    const KeyDescription & primary_key)
{
    TTLDescription result;
    const auto * ttl_element = definition_ast->as<ASTTTLElement>();

    /// First child is expression: `TTL expr TO DISK`
    if (ttl_element != nullptr)
        result.expression_ast = ttl_element->children.front()->clone();
    else /// It's columns TTL without any additions, just copy it
        result.expression_ast = definition_ast->clone();

    auto ttl_ast = result.expression_ast->clone();
    auto syntax_analyzer_result = TreeRewriter(context).analyze(ttl_ast, columns.getAllPhysical());
    result.expression = ExpressionAnalyzer(ttl_ast, syntax_analyzer_result, context).getActions(false);
    result.result_column = ttl_ast->getColumnName();

    if (ttl_element == nullptr) /// columns TTL
    {
        result.destination_type = DataDestinationType::DELETE;
        result.mode = TTLMode::DELETE;
    }
    else /// rows TTL
    {
        result.destination_type = ttl_element->destination_type;
        result.destination_name = ttl_element->destination_name;
        result.mode = ttl_element->mode;

        if (ttl_element->mode == TTLMode::DELETE)
        {
            if (ASTPtr where_expr_ast = ttl_element->where())
            {
                auto where_syntax_result = TreeRewriter(context).analyze(where_expr_ast, columns.getAllPhysical());
                result.where_expression = ExpressionAnalyzer(where_expr_ast, where_syntax_result, context).getActions(false);
                result.where_result_column = where_expr_ast->getColumnName();
            }
        }
        else if (ttl_element->mode == TTLMode::GROUP_BY)
        {
            const auto & pk_columns = primary_key.column_names;

            if (ttl_element->group_by_key.size() > pk_columns.size())
                throw Exception("TTL Expression GROUP BY key should be a prefix of primary key", ErrorCodes::BAD_TTL_EXPRESSION);

            NameSet aggregation_columns_set;
            NameSet used_primary_key_columns_set;

            for (size_t i = 0; i < ttl_element->group_by_key.size(); ++i)
            {
                if (ttl_element->group_by_key[i]->getColumnName() != pk_columns[i])
                    throw Exception(
                        "TTL Expression GROUP BY key should be a prefix of primary key",
                        ErrorCodes::BAD_TTL_EXPRESSION);

                used_primary_key_columns_set.insert(pk_columns[i]);
            }

            for (const auto & [name, _] : ttl_element->group_by_aggregations)
                aggregation_columns_set.insert(name);

            if (aggregation_columns_set.size() != ttl_element->group_by_aggregations.size())
                throw Exception(
                    "Multiple aggregations set for one column in TTL Expression",
                    ErrorCodes::BAD_TTL_EXPRESSION);

            result.group_by_keys = Names(pk_columns.begin(), pk_columns.begin() + ttl_element->group_by_key.size());
            auto aggregations = ttl_element->group_by_aggregations;

            const auto & primary_key_expressions = primary_key.expression_list_ast->children;
            for (size_t i = ttl_element->group_by_key.size(); i < primary_key_expressions.size(); ++i)
            {
                ASTPtr expr = makeASTFunction("any", primary_key_expressions[i]->clone());
                aggregations.emplace_back(pk_columns[i], std::move(expr));
                aggregation_columns_set.insert(pk_columns[i]);
            }

            for (const auto & column : columns.getOrdinary())
            {
                if (!aggregation_columns_set.count(column.name) && !used_primary_key_columns_set.count(column.name))
                {
                    ASTPtr expr = makeASTFunction("any", std::make_shared<ASTIdentifier>(column.name));
                    aggregations.emplace_back(column.name, std::move(expr));
                }
            }

            for (auto [name, value] : aggregations)
            {
                auto syntax_result = TreeRewriter(context).analyze(value, columns.getAllPhysical(), {}, {}, true);
                auto expr_analyzer = ExpressionAnalyzer(value, syntax_result, context);

                TTLAggregateDescription set_part;
                set_part.column_name = name;
                set_part.expression_result_column_name = value->getColumnName();
                set_part.expression = expr_analyzer.getActions(false);

                result.set_parts.emplace_back(set_part);

                for (const auto & descr : expr_analyzer.getAnalyzedData().aggregate_descriptions)
                    result.aggregate_descriptions.push_back(descr);
            }
        }
        else if (ttl_element->mode == TTLMode::RECOMPRESS)
        {
            result.recompression_codec =
                CompressionCodecFactory::instance().validateCodecAndGetPreprocessedAST(
                    ttl_element->recompression_codec, {}, !context.getSettingsRef().allow_suspicious_codecs);
        }
    }

    checkTTLExpression(result.expression, result.result_column);
    return result;
}


TTLTableDescription::TTLTableDescription(const TTLTableDescription & other)
 : definition_ast(other.definition_ast ? other.definition_ast->clone() : nullptr)
 , rows_ttl(other.rows_ttl)
 , move_ttl(other.move_ttl)
 , recompression_ttl(other.recompression_ttl)
 , group_by_ttl(other.group_by_ttl)
{
}

TTLTableDescription & TTLTableDescription::operator=(const TTLTableDescription & other)
{
    if (&other == this)
        return *this;

    if (other.definition_ast)
        definition_ast = other.definition_ast->clone();
    else
        definition_ast.reset();

    rows_ttl = other.rows_ttl;
    move_ttl = other.move_ttl;
    recompression_ttl = other.recompression_ttl;
    group_by_ttl = other.group_by_ttl;

    return *this;
}

TTLTableDescription TTLTableDescription::getTTLForTableFromAST(
    const ASTPtr & definition_ast,
    const ColumnsDescription & columns,
    const Context & context,
    const KeyDescription & primary_key)
{
    TTLTableDescription result;
    if (!definition_ast)
        return result;

    result.definition_ast = definition_ast->clone();

    bool seen_delete_ttl = false;
    for (const auto & ttl_element_ptr : definition_ast->children)
    {
        auto ttl = TTLDescription::getTTLFromAST(ttl_element_ptr, columns, context, primary_key);
        if (ttl.mode == TTLMode::DELETE)
        {
            if (seen_delete_ttl)
                throw Exception("More than one DELETE TTL expression is not allowed", ErrorCodes::BAD_TTL_EXPRESSION);
            result.rows_ttl = ttl;
            seen_delete_ttl = true;
        }
        else if (ttl.mode == TTLMode::RECOMPRESS)
        {
            result.recompression_ttl.emplace_back(std::move(ttl));
        }
        else if (ttl.mode == TTLMode::GROUP_BY)
        {
            result.group_by_ttl.emplace_back(std::move(ttl));
        }
        else
        {
            result.move_ttl.emplace_back(std::move(ttl));
        }
    }
    return result;
}

}
