namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>Lengths for transform translations and origins.</summary>
        public static class Length
        {
            /// <summary>A length in pixels.</summary>
            public static UiLength Px(float value) => new() { Value = value };

            /// <summary>A percentage of the element's border box; 100 means 100%.</summary>
            public static UiLength Percent(float value) => new() { Value = value, IsPercent = true };
        }
    }
}
