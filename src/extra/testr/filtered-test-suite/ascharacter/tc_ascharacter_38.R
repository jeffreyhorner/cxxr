expected <- eval(parse(text="c(\"2001-01-01\", \"2001-01-08\", \"2001-01-15\", \"2001-01-22\", \"2001-01-29\", \"2001-02-05\", \"2001-02-12\", \"2001-02-19\", \"2001-02-26\", \"2001-03-05\")"));                 
test(id=0, code={                 
argv <- eval(parse(text="list(structure(c(978307200, 978912000, 979516800, 980121600, 980726400, 981331200, 981936000, 982540800, 983145600, 983750400), class = c(\"POSIXct\", \"POSIXt\"), tzone = \"GMT\"))"));                 
do.call(`as.character`, argv);                 
}, o=expected);                 

