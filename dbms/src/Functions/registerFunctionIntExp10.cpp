#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsArithmetic.h>
#include <Functions/registerFunctionIntExp10.h>

namespace DB
{
void registerFunctionIntExp10(FunctionFactory & factory)
{
    factory.registerFunction<FunctionIntExp10>();
}

} // namespace DB
