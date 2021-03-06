% Generated by roxygen2 (4.1.0): do not edit by hand
% Please edit documentation in R/testr.r
\name{is.generator}
\alias{is.generator}
\title{Determines if given object is an instance of generator.

A generator must inherit from a generator class. Likely this is produced by calling either customGenerator, or generator function.}
\usage{
is.generator(o)
}
\arguments{
\item{o}{Object to be tested.}
}
\value{
TRUE if the argument is a generator, FALSE otherwise
}
\description{
Determines if given object is an instance of generator.

A generator must inherit from a generator class. Likely this is produced by calling either customGenerator, or generator function.
}
\examples{
is.generator(1) # FALSE
is.generator(generator(a, 1, 2, 3)) # TRUE
}
\seealso{
customGenerator, generator
}

