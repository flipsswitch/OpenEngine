using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>The generated transform binding and event ABI against the real native UI tree.</summary>
    [NonParallelizable]
    public class UiTransformBindingTests
    {
        private const string kScene = "BindingTestScene";

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern ulong GE_TestUI_Open();

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern void GE_TestUI_Close();

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern void GE_TestUI_ReadWindowRect(out float x, out float y, out float width, out float height);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern void GE_TestUI_DispatchPointer(float x, float y);

        private Ui.Element m_Element = null!;

        /// <summary>A generated call starts the standalone engine before the UI host is published.</summary>
        [OneTimeSetUp]
        public void StartEngine() => Input.GetMousePosition();

        /// <summary>Each test owns a new game UI tree.</summary>
        [SetUp]
        public void Open()
        {
            ulong id = GE_TestUI_Open();
            Assert.That(id, Is.Not.Zero);
            m_Element = new Ui.Element(id);
        }

        /// <summary>Destroy the UI tree and unpublish its host.</summary>
        [TearDown]
        public void Close() => GE_TestUI_Close();

        private static void ExpectWindowRect(float x, float y, float width, float height)
        {
            GE_TestUI_ReadWindowRect(out float actualX, out float actualY, out float actualWidth, out float actualHeight);
            Assert.Multiple(() =>
            {
                Assert.That(actualX, Is.EqualTo(x).Within(0.001f));
                Assert.That(actualY, Is.EqualTo(y).Within(0.001f));
                Assert.That(actualWidth, Is.EqualTo(width).Within(0.001f));
                Assert.That(actualHeight, Is.EqualTo(height).Within(0.001f));
            });
        }

        /// <summary>Rotation and a mixed-unit pivot change the real element bounds.</summary>
        [Test]
        public void RotationAndMixedUnitOriginReachTheNativeElement()
        {
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(0), Ui.Length.Percent(100)), Is.True);
            Assert.That(m_Element.SetTransform([Ui.Transform.Rotate(90)]), Is.True);
            ExpectWindowRect(40, 90, 40, 100);
            Assert.That(m_Element.ClearTransformOrigin(), Is.True);
            ExpectWindowRect(70, 20, 40, 100);
        }

        /// <summary>Every transform kind crosses the generated record/span boundary in CSS order.</summary>
        [Test]
        public void OrderedOperationsReachTheNativeElement()
        {
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(0), Ui.Length.Px(0)), Is.True);
            Assert.That(m_Element.SetTransform([
                Ui.Transform.Translate(Ui.Length.Percent(50), Ui.Length.Px(10)),
                Ui.Transform.Scale(2, 3)]), Is.True);
            ExpectWindowRect(90, 60, 200, 120);
            Assert.That(m_Element.SetTransform([Ui.Transform.Skew(45, 0)]), Is.True);
            ExpectWindowRect(40, 50, 140, 40);
            Assert.That(m_Element.SetTransform([Ui.Transform.Matrix(2, 0, 0, 3, 7, 11)]), Is.True);
            ExpectWindowRect(47, 61, 200, 120);
        }

        /// <summary>A borrowed slice is pinned at its start, and native styles own a copy after the call.</summary>
        [Test]
        public void BorrowedArraySliceIsCopiedBeforeReturning()
        {
            UiTransformOperation[] operations = [Ui.Transform.Rotate(float.NaN),
                Ui.Transform.Translate(Ui.Length.Px(50), Ui.Length.Px(10)), Ui.Transform.Scale(2, 3),
                Ui.Transform.Rotate(float.NaN)];
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(0), Ui.Length.Px(0)), Is.True);
            Assert.That(m_Element.SetTransform(operations.AsSpan(1, 2)), Is.True);
            operations[1] = Ui.Transform.Rotate(90);
            ExpectWindowRect(90, 60, 200, 120);
        }

        /// <summary>An empty list suppresses authored transforms; clearing restores them.</summary>
        [Test]
        public void EmptyListSuppressesTheAuthoredTransformAndClearRestoresIt()
        {
            ExpectWindowRect(50, 70, 100, 40);
            Assert.That(m_Element.SetTransform([]), Is.True);
            ExpectWindowRect(40, 50, 100, 40);
            Assert.That(m_Element.ClearTransform(), Is.True);
            ExpectWindowRect(50, 70, 100, 40);
        }

        /// <summary>Invalid values leave the previous style intact, and stale ids are harmless.</summary>
        [Test]
        public void InvalidInputAndDeadElementsDoNotChangeTheTransform()
        {
            Assert.That(m_Element.SetTransform([Ui.Transform.Rotate(float.NaN)]), Is.False);
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(float.PositiveInfinity), Ui.Length.Px(0)), Is.False);
            ExpectWindowRect(50, 70, 100, 40);
            GE_TestUI_Close();
            Assert.That(m_Element.SetTransform([Ui.Transform.Rotate(30)]), Is.False);
            Assert.That(m_Element.ClearTransform(), Is.False);
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(0), Ui.Length.Px(0)), Is.False);
            Assert.That(m_Element.ClearTransformOrigin(), Is.False);
        }

        /// <summary>A transformed point preserves both layout and window coordinates through the callback.</summary>
        [Test]
        public void PointerCallbackKeepsItsWindowPointUnderATransform()
        {
            Assert.That(m_Element.SetTransformOrigin(Ui.Length.Px(0), Ui.Length.Px(0)), Is.True);
            Assert.That(m_Element.SetTransform([Ui.Transform.Scale(2, 2)]), Is.True);
            ExpectWindowRect(40, 50, 200, 80);
            Ui.PointerArgs received = default;
            int calls = 0;
            Action<Ui.PointerArgs> onMove = position => { received = position; ++calls; };
            m_Element.MouseMoved += onMove;
            try { GE_TestUI_DispatchPointer(80, 90); }
            finally { m_Element.MouseMoved -= onMove; }
            Assert.That(calls, Is.EqualTo(1));
            Assert.That(received.X, Is.EqualTo(60));
            Assert.That(received.Y, Is.EqualTo(70));
            Assert.That(received.WindowX, Is.EqualTo(80));
            Assert.That(received.WindowY, Is.EqualTo(90));
        }

    }
}
