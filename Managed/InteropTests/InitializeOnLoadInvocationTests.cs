using System;
using System.Linq;
using System.Runtime.Loader;
using System.Reflection;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Tests that [InitializeOnLoad] methods are discovered during preload but only invoked after Stage 4 swap.
    /// </summary>
    public class InitializeOnLoadInvocationTests
    {
        /// <summary>
        /// Preload must not invoke IoL; swap must invoke IoL exactly once.
        /// </summary>
        [Test]
        public void Preload_DoesNotInvoke_IoL_OnlyAfterSwap()
        {
            ScriptCompileHelper.EnsureHrmLoaded();
            var coreBridge = Type.GetType("GameEngine.CoreBridge.CoreBridge, GameEngine.CoreBridge", throwOnError: false);
            coreBridge?.GetMethod("Demo_InitializeManaged", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);

            var src = "using System;\nnamespace GameEngine.Scripting { [AttributeUsage(AttributeTargets.Method, AllowMultiple=false, Inherited=false)] public sealed class InitializeOnLoadAttribute : Attribute { } }\nnamespace TestScripts { public static class Init { public static int Marker = 0; [GameEngine.Scripting.InitializeOnLoad] public static void Boot() { Marker = Marker + 1; } } }";
            var assemblyName = "InitializeOnLoad_" + Guid.NewGuid().ToString("N");
            var (asm, pdb) = ScriptCompileHelper.CompileCustomSources(src, assemblyName);

            var hrm = Type.GetType("GameEngine.HotReload.HotReloadManager, GameEngine.HotReload", throwOnError: true)!;
            var miPreload = hrm.GetMethod("PreloadAssemblyContext", BindingFlags.Public | BindingFlags.Static, binder: null, types: new[]{ typeof(byte[]), typeof(byte[]) }, modifiers: null)!;
            var miSwap = hrm.GetMethod("SwapPreloadedContext", BindingFlags.Public | BindingFlags.Static)!;


            int preRc = (int)miPreload.Invoke(null, new object?[]{ asm, pdb })!;
            Assert.That(preRc, Is.EqualTo(0), "Preload failed");
            // Inspect the unique preloaded assembly through the runtime in every configuration.
            var preAsm = AssemblyLoadContext.All.SelectMany(context => context.Assemblies)
                .Single(assembly => assembly.GetName().Name == assemblyName);
            Assert.That(preAsm, Is.Not.Null, "Preloaded assembly missing after preload");
            var miDiscover = hrm.GetMethod("DiscoverInitializeOnLoadMethods", BindingFlags.Static | BindingFlags.NonPublic);
            var discovered = (System.Collections.Generic.List<MethodInfo>)miDiscover!.Invoke(null, new object?[]{ preAsm!, null })!;
            Assert.That(discovered.Count, Is.GreaterThanOrEqualTo(1), "No [InitializeOnLoad] methods discovered during preload");

            // Read Marker before swap (should be 0)
            var initTypePre = preAsm!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerFieldPre = initTypePre.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            var markerPre = (int)markerFieldPre.GetValue(null)!;
            Assert.That(markerPre, Is.EqualTo(0), "Marker should be 0 before swap");

            int swapRc = (int)miSwap.Invoke(null, null)!;
            Assert.That(swapRc, Is.EqualTo(0), "Swap failed");

            // Await IoL completion without polling
            var miWaitPending = hrm.GetMethod("WaitForPendingInitializeOnLoadAsync", BindingFlags.Public | BindingFlags.Static)!;
            var t = (System.Threading.Tasks.Task<bool>)miWaitPending.Invoke(null, new object?[]{ 5000 })!;
            Assert.That(t.GetAwaiter().GetResult(), Is.True, "InitializeOnLoad did not complete within timeout");

            // Read the active assembly after swap; a stale collectible context cannot satisfy this lookup.
            var currAsm = GameEngine.HotReload.HotReloadManager.GetActiveUserAssemblies()
                .Single(assembly => assembly.GetName().Name == assemblyName);
            Assert.That(currAsm, Is.Not.Null, "Current assembly missing after swap");
            var initType = currAsm!.GetType("TestScripts.Init", throwOnError: true)!;
            var markerField = initType.GetField("Marker", BindingFlags.Public | BindingFlags.Static)!;
            int marker = (int)markerField.GetValue(null)!;
            Assert.That(marker, Is.EqualTo(1), "InitializeOnLoad should set Marker to 1 after swap");
        }
    }
}

