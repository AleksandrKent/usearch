using System;

namespace Cloud.Unum.USearch;

internal class Utils
{
    internal static ScalarKind GetScalarKind(Type dotnetType)
    {
        if (dotnetType == typeof(float))
        {
            return ScalarKind.Float32;
        }
        else if (dotnetType == typeof(double))
        {
            return ScalarKind.Float64;
        }
        else
        {
            throw new NotSupportedException($"{nameof(dotnetType)}");
        }
    }
}
