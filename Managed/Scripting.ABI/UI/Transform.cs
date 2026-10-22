namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>Typed functions for an element's CSS transform list.</summary>
        public static class Transform
        {
            /// <summary>Translate in pixels or percent of the element's border box.</summary>
            public static UiTransformOperation Translate(UiLength x, UiLength y)
                => new()
                {
                    Function = TransformFunction.Translate,
                    A = x.Value,
                    B = y.Value,
                    XIsPercent = x.IsPercent,
                    YIsPercent = y.IsPercent
                };

            /// <summary>Scale each axis by its factor.</summary>
            public static UiTransformOperation Scale(float x, float y)
                => new() { Function = TransformFunction.Scale, A = x, B = y };

            /// <summary>Rotate in degrees, positive clockwise.</summary>
            public static UiTransformOperation Rotate(float degrees)
                => new() { Function = TransformFunction.Rotate, A = degrees };

            /// <summary>Skew each axis in degrees.</summary>
            public static UiTransformOperation Skew(float xDegrees, float yDegrees)
                => new() { Function = TransformFunction.Skew, A = xDegrees, B = yDegrees };

            /// <summary>The six CSS matrix values; e and f translate in pixels.</summary>
            public static UiTransformOperation Matrix(float a, float b, float c, float d, float e, float f)
                => new() { Function = TransformFunction.Matrix, A = a, B = b, C = c, D = d, E = e, F = f };
        }
    }
}
