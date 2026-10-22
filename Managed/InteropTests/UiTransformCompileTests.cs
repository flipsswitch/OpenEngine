using System;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Compile a user-facing example against the shipped managed assemblies.</summary>
    public class UiTransformCompileTests
    {
        /// <summary>The typed helpers and both pointer coordinate spaces are available to scripts.</summary>
        [Test]
        public void ScriptCanSetAndClearTransformsAndReadBothPointerSpaces()
        {
            const string kSource = """
                using GameEngine.Scripting;

                public static class TransformScript
                {
                    public static void Update(Ui.Element needle, float degrees)
                    {
                        needle.SetTransform([
                            Ui.Transform.Translate(Ui.Length.Percent(-50), Ui.Length.Px(4)),
                            Ui.Transform.Rotate(degrees),
                            Ui.Transform.Scale(2, 0.5f),
                            Ui.Transform.Skew(10, 20),
                            Ui.Transform.Matrix(1, 0, 0, 1, 3, 5)]);
                        needle.SetTransformOrigin(Ui.Length.Percent(50), Ui.Length.Percent(100));
                        needle.SetTransform([]);
                        needle.ClearTransform();
                        needle.ClearTransformOrigin();
                        needle.MouseMoved += position =>
                        {
                            float x = position.WindowX - position.X;
                            float y = position.WindowY - position.Y;
                        };
                    }
                }
                """;

            Assert.DoesNotThrow(() => ScriptCompileHelper.CompileCustomSources(kSource,
                extraReferencePaths: new[] { typeof(Ui).Assembly.Location, typeof(Input).Assembly.Location }));
        }
    }
}
