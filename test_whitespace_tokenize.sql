-- Test cases for whitespace_tokenize function

-- Test 1: Basic tokenization
SELECT whitespace_tokenize("I am apple");

-- Test 2: Multiple spaces
SELECT whitespace_tokenize("  a   b  c  ");

-- Test 3: Empty string
SELECT whitespace_tokenize("");

-- Test 4: NULL input
SELECT whitespace_tokenize(NULL);

-- Test 5: Single word
SELECT whitespace_tokenize("hello");

-- Test 6: Tabs and newlines
SELECT whitespace_tokenize("line1	tab	line2
newline");

