namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;MarkdownText&gt; control. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class MarkdownText : Element
        {
            internal MarkdownText(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing Markdown text control.</summary>
            protected MarkdownText(MarkdownText source)
                : base(source)
            {
            }

            /// <summary>Set Markdown text on the main thread. Null or empty text clears the control.</summary>
            /// <returns>False when the element no longer resolves.</returns>
            public bool SetText(string? text)
                => GE_UIElement_SetMarkdownText(InstanceId, text ?? string.Empty) == 0;
        }
    }
}
