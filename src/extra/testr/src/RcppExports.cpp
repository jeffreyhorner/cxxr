// This file was generated by Rcpp::compileAttributes
// Generator token: 10BE3573-1514-4C36-9D1C-5A225CD40393

#include "../inst/include/testr.h"
#include <Rcpp.h>

using namespace Rcpp;

// try_eval_tracer
void try_eval_tracer(SEXP env);
RcppExport SEXP testr_try_eval_tracer(SEXP envSEXP) {
BEGIN_RCPP
    {
        Rcpp::RNGScope __rngScope;
        Rcpp::traits::input_parameter< SEXP >::type env(envSEXP );
        try_eval_tracer(env);
    }
    return R_NilValue;
END_RCPP
}
// WriteCapInfo_cpp
void WriteCapInfo_cpp(CharacterVector fname, SEXP args_env);
RcppExport SEXP testr_WriteCapInfo_cpp(SEXP fnameSEXP, SEXP args_envSEXP) {
BEGIN_RCPP
    {
        Rcpp::RNGScope __rngScope;
        Rcpp::traits::input_parameter< CharacterVector >::type fname(fnameSEXP );
        Rcpp::traits::input_parameter< SEXP >::type args_env(args_envSEXP );
        WriteCapInfo_cpp(fname, args_env);
    }
    return R_NilValue;
END_RCPP
}
