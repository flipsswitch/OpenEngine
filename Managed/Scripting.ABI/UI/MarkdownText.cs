namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        public class MarkdownText : Element
        {
            internal MarkdownText(ulong instanceId)
                : base(instanceId)
            {
            }

            protected MarkdownText(MarkdownText source)
                : base(source)
            {
            }

            public bool SetText(string? text)
                => GE_UIElement_SetMarkdownText(InstanceId, text ?? string.Empty) == 0;
        }
    }
}
