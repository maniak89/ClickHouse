SELECT 'Tuple';

DROP TABLE IF EXISTS test_tuple;
CREATE TABLE test_tuple (value Tuple(UInt8, UInt8)) ENGINE=TinyLog;

SET input_format_null_as_default = 1;
INSERT INTO test_tuple VALUES ((NULL, 1));
SELECT * FROM test_tuple;

SET input_format_null_as_default = 0;
INSERT INTO test_tuple VALUES ((NULL, 2)); -- { clientError 53 }
SELECT * FROM test_tuple;

DROP TABLE test_tuple;

SELECT 'Tuple nested in Array';

DROP TABLE IF EXISTS test_tuple_nested_in_array;
CREATE TABLE test_tuple_nested_in_array (value Array(Tuple(UInt8, UInt8))) ENGINE=TinyLog;

SET input_format_null_as_default = 1;
INSERT INTO test_tuple_nested_in_array VALUES ([(NULL, 1)]);
SELECT * FROM test_tuple_nested_in_array;

SET input_format_null_as_default = 0;
INSERT INTO test_tuple_nested_in_array VALUES ([(NULL, 1)]); -- { clientError 53 }
SELECT * FROM test_tuple_nested_in_array;

DROP TABLE test_tuple_nested_in_array;

SELECT 'Tuple nested in Array nested in Tuple';

DROP TABLE IF EXISTS test_tuple_nested_in_array_nested_in_tuple;
CREATE TABLE test_tuple_nested_in_array_nested_in_tuple (value Tuple(UInt8, Array(Tuple(UInt8, UInt8)))) ENGINE=TinyLog;

SET input_format_null_as_default = 1;
INSERT INTO test_tuple_nested_in_array_nested_in_tuple VALUES ( (NULL, [(NULL, 1)]) );
SELECT * FROM test_tuple_nested_in_array_nested_in_tuple;

SET input_format_null_as_default = 0;
INSERT INTO test_tuple_nested_in_array_nested_in_tuple VALUES ( (NULL, [(NULL, 1)]) ); -- { clientError 53 }
SELECT * FROM test_tuple_nested_in_array_nested_in_tuple;

DROP TABLE test_tuple_nested_in_array_nested_in_tuple;
