expected <- eval(parse(text="FALSE"));     
test(id=0, code={     
argv <- eval(parse(text="list(numeric(0))"));     
do.call(`is.symbol`, argv);     
}, o=expected);     

