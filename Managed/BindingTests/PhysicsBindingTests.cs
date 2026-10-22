using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated Physics binding through the real GameEngine.Native: a ray cast, the velocity of a body, and the
    /// contact events with their subscription rules. The dotnet test host has no frame loop, so BindingTestScene
    /// (Tests/ScriptBindings) builds the bodies and runs the physics steps, which deliver the events on this thread.
    /// </summary>
    [NonParallelizable]
    public class PhysicsBindingTests
    {
        private const string kScene = "BindingTestScene";

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Open();

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_AddBox(float x, float y, float z, float halfX, float halfY, float halfZ,
                                                      BoxKind kind, out uint entity);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Step(int steps);

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_Close();

        // GE_TestScene_AddBox's kind.
        private enum BoxKind
        {
            Static = 0,
            Dynamic = 1,
            Trigger = 2,
        }

        private int m_Begins;
        private int m_Ends;
        private int m_Unsubscribed;
        private ContactEvent m_LastBegin;
        private readonly List<TriggerEvent> m_Enters = new List<TriggerEvent>();

        /// <summary>A generated call starts the standalone engine the scene needs.</summary>
        [OneTimeSetUp]
        public void StartEngine()
        {
            Assert.That(Physics.RayCast(Vector3.Zero, Vector3.UnitZ, out RayHit outside), Is.False);
            Assert.That(outside.Entity, Is.EqualTo(Entity.None), "outside play the hit holds its defaults");
        }

        /// <summary>Each test starts a physics world of its own.</summary>
        [SetUp]
        public void OpenScene()
        {
            Assert.That(GE_TestScene_Open(), Is.EqualTo(0));
            m_Begins = 0;
            m_Ends = 0;
            m_Unsubscribed = 0;
            m_Enters.Clear();
        }

        /// <summary>Leaving play mode shuts the world down.</summary>
        [TearDown]
        public void CloseScene()
        {
            Assert.That(GE_TestScene_Close(), Is.EqualTo(0));
        }

        private static Entity AddBox(Vector3 position, Vector3 halfExtents, BoxKind kind)
        {
            Assert.That(GE_TestScene_AddBox(position.X, position.Y, position.Z, halfExtents.X, halfExtents.Y, halfExtents.Z,
                                            kind, out uint id), Is.EqualTo(0));
            return new Entity(id);
        }

        private static Entity AddGround() => AddBox(new Vector3(0f, -0.5f, 0f), new Vector3(10f, 0.5f, 10f), BoxKind.Static);

        private void OnBegin(in ContactEvent e)
        {
            ++m_Begins;
            m_LastBegin = e;
            // Launch the box off the ground from inside the step, so the contact ends.
            e.Self.Physics.SetLinearVelocity(new Vector3(0f, 8f, 0f));
        }

        private void OnEnd(in ContactEvent e) => ++m_Ends;

        private void OnUnsubscribed(in ContactEvent e) => ++m_Unsubscribed;

        private void OnEnter(in TriggerEvent e) => m_Enters.Add(e);

        /// <summary>A ray along +Z hits the box in front of it, and a ray along -Z hits nothing.</summary>
        [Test]
        public void RayCastHitsTheBoxInFrontAndMissesBehind()
        {
            Entity box = AddBox(new Vector3(0f, 0f, 10f), Vector3.One, BoxKind.Static);

            Assert.That(Physics.RayCast(Vector3.Zero, new Vector3(0f, 0f, 2f), out RayHit hit), Is.True);
            Assert.That(hit.Entity, Is.EqualTo(box));
            Assert.That(hit.Distance, Is.EqualTo(9f).Within(1e-3f));
            Assert.That(hit.Point.Z, Is.EqualTo(9f).Within(1e-3f));
            Assert.That(hit.Normal.Z, Is.EqualTo(-1f).Within(1e-3f));

            Assert.That(Physics.RayCast(Vector3.Zero, -Vector3.UnitZ, out RayHit miss), Is.False);
            Assert.That(miss.Entity, Is.EqualTo(Entity.None));
            Assert.That(Physics.RayCast(Vector3.Zero, Vector3.UnitZ, out _, maxDistance: 5f), Is.False, "beyond maxDistance");
        }

        /// <summary>
        /// A box falls onto the ground: ContactBegin fires once when it lands, its handler launches it upward through the
        /// view's velocity, and ContactEnd fires once when it leaves.
        /// </summary>
        [Test]
        public void ContactBeginFiresOnceOnContactAndContactEndOnceOnSeparation()
        {
            Entity ground = AddGround();
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnBegin;
            box.Physics.ContactEnd += OnEnd;

            Assert.That(GE_TestScene_Step(60), Is.EqualTo(0));
            Assert.That(m_Begins, Is.EqualTo(1));
            Assert.That(m_Ends, Is.EqualTo(1));
            Assert.That(m_LastBegin.Self, Is.EqualTo(box));
            Assert.That(m_LastBegin.Other, Is.EqualTo(ground));
            Assert.That(m_LastBegin.Normal.Y, Is.GreaterThan(0.5f), "the normal points from the ground up into the box");
            Assert.That(box.Physics.GetLinearVelocity().Y, Is.GreaterThan(0f), "the box is still rising");

            box.Physics.ContactBegin -= OnBegin;
            box.Physics.ContactEnd -= OnEnd;
        }

        /// <summary>
        /// A box falls into a trigger that lies on the ground: TriggerEnter reaches the trigger's entity and the box's, each
        /// as Self. The steps stop soon after the box lands, before it rests long enough to sleep.
        /// </summary>
        [Test]
        public void TriggerEnterReachesTheTriggerAndTheBody()
        {
            AddGround();
            Entity trigger = AddBox(Vector3.Zero, new Vector3(2f, 0.5f, 2f), BoxKind.Trigger);
            Entity box = AddBox(new Vector3(0f, 3f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            trigger.Physics.TriggerEnter += OnEnter;
            box.Physics.TriggerEnter += OnEnter;

            Assert.That(GE_TestScene_Step(45), Is.EqualTo(0));

            Assert.That(m_Enters.Exists(e => e.Self == trigger && e.Other == box && e.SelfIsTrigger), Is.True);
            Assert.That(m_Enters.Exists(e => e.Self == box && e.Other == trigger && !e.SelfIsTrigger), Is.True);
            trigger.Physics.TriggerEnter -= OnEnter;
            box.Physics.TriggerEnter -= OnEnter;
        }

        /// <summary>A handler removed with -= is not called; the one left on the same event is.</summary>
        [Test]
        public void UnsubscribedHandlerIsNotCalled()
        {
            AddGround();
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnUnsubscribed;
            box.Physics.ContactBegin += OnBegin;
            box.Physics.ContactBegin -= OnUnsubscribed;

            Assert.That(GE_TestScene_Step(60), Is.EqualTo(0));

            Assert.That(m_Unsubscribed, Is.Zero);
            Assert.That(m_Begins, Is.EqualTo(1));
            box.Physics.ContactBegin -= OnBegin;
        }

        /// <summary>
        /// Leaving play mode (the physics world shuts down) ends the subscription: the binding's listener is gone, and a
        /// later -= of the same handler does nothing.
        /// </summary>
        [Test]
        public void SubscriptionDoesNotSurviveTheSceneUnload()
        {
            int before = ScriptSubscriptionRegistry.Count;
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            box.Physics.ContactBegin += OnBegin;
            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before + 1));

            Assert.That(GE_TestScene_Close(), Is.EqualTo(0));

            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before));
            Assert.DoesNotThrow(() => box.Physics.ContactBegin -= OnBegin);
        }

        /// <summary>
        /// A handler whose code lives in a collectible scripts context (a hot-reloaded script) is dropped and its
        /// subscription ended when that context unloads, and the context is then collected.
        /// </summary>
        [Test]
        public void UnloadingTheScriptContextEndsItsSubscriptions()
        {
            Entity box = AddBox(new Vector3(0f, 1f, 0f), new Vector3(0.5f, 0.5f, 0.5f), BoxKind.Dynamic);
            int before = ScriptSubscriptionRegistry.Count;

            WeakReference context = SubscribeFromCollectibleContext(box, out int subscribed);
            Assert.That(subscribed, Is.EqualTo(before + 1), "the script's listener before the unload");

            for (int i = 0; i < 20 && context.IsAlive; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            Assert.That(ScriptSubscriptionRegistry.Count, Is.EqualTo(before), "the engine ended the subscription");
            Assert.That(context.IsAlive, Is.False, "nothing roots the unloaded context");
        }

        /// <summary>The nearest hits fill only the supplied slice, in distance order.</summary>
        [Test]
        public void RayCastAllReturnsNearestHitsWithinTheBuffer()
        {
            AddBox(new Vector3(0f, 0f, 12f), Vector3.One, BoxKind.Static);
            Entity nearest = AddBox(new Vector3(0f, 0f, 4f), Vector3.One, BoxKind.Static);
            Entity next = AddBox(new Vector3(0f, 0f, 8f), Vector3.One, BoxKind.Static);
            var hits = new RayHit[4];
            Array.Fill(hits, new RayHit { Distance = -1f });

            int count = Physics.RayCastAll(Vector3.Zero, new Vector3(0f, 0f, 2f), hits.AsSpan(1, 2));

            Assert.That(count, Is.EqualTo(2));
            Assert.That(hits[1].Entity, Is.EqualTo(nearest));
            Assert.That(hits[2].Entity, Is.EqualTo(next));
            Assert.That(hits[1].Distance, Is.EqualTo(3f).Within(1e-3f));
            Assert.That(hits[2].Distance, Is.EqualTo(7f).Within(1e-3f));
            Assert.That(hits[1].Point.X, Is.EqualTo(0f).Within(1e-3f));
            Assert.That(hits[1].Point.Y, Is.EqualTo(0f).Within(1e-3f));
            Assert.That(hits[1].Point.Z, Is.EqualTo(3f).Within(1e-3f));
            Assert.That(hits[1].Normal, Is.EqualTo(-Vector3.UnitZ));
            Assert.That(hits[0].Distance, Is.EqualTo(-1f));
            Assert.That(hits[3].Distance, Is.EqualTo(-1f));
        }

        /// <summary>The ray query applies its layer, trigger, and distance filters.</summary>
        [Test]
        public void RayCastAllAppliesTheNativeFilters()
        {
            Entity solid = AddBox(new Vector3(0f, 0f, 4f), Vector3.One, BoxKind.Static);
            Entity trigger = AddBox(new Vector3(0f, 0f, 8f), Vector3.One, BoxKind.Trigger);
            Entity dynamicBox = AddBox(new Vector3(0f, 0f, 12f), Vector3.One, BoxKind.Dynamic);
            Span<RayHit> hits = stackalloc RayHit[4];
            hits.Fill(new RayHit { Distance = -1f });

            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits), Is.EqualTo(3));
            Assert.That(hits[0].Entity, Is.EqualTo(solid));
            Assert.That(hits[1].Entity, Is.EqualTo(trigger));
            Assert.That(hits[2].Entity, Is.EqualTo(dynamicBox));
            Assert.That(hits[3].Distance, Is.EqualTo(-1f));
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, ignoreTriggers: true), Is.EqualTo(2));
            Assert.That(hits[0].Entity, Is.EqualTo(solid));
            Assert.That(hits[1].Entity, Is.EqualTo(dynamicBox));
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, layerMask: 1u << 1), Is.EqualTo(1));
            Assert.That(hits[0].Entity, Is.EqualTo(dynamicBox));
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: 5f), Is.EqualTo(1));
            Assert.That(hits[0].Entity, Is.EqualTo(solid));
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, layerMask: 0), Is.Zero);
        }

        /// <summary>An empty buffer, a miss, or an invalid ray writes no result.</summary>
        [Test]
        public void RayCastAllLeavesTheBufferAloneWhenThereAreNoHits()
        {
            AddBox(new Vector3(0f, 0f, 4f), Vector3.One, BoxKind.Static);
            Span<RayHit> hits = stackalloc RayHit[1];
            hits[0] = new RayHit { Distance = -1f };

            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, Span<RayHit>.Empty), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, -Vector3.UnitZ, hits), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.Zero, hits), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: 0f), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: float.NaN), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: -1f), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, hits, maxDistance: float.PositiveInfinity), Is.Zero);
            Assert.That(Physics.RayCastAll(new Vector3(float.NaN, 0f, 0f), Vector3.UnitZ, hits), Is.Zero);
            Assert.That(Physics.RayCastAll(Vector3.Zero, new Vector3(float.NaN, 0f, 0f), hits), Is.Zero);
            Assert.That(hits[0].Distance, Is.EqualTo(-1f));
        }

        /// <summary>The box orientation reaches the native query in the engine's coordinate convention.</summary>
        [Test]
        public void BoxOverlapUsesTheSuppliedRotation()
        {
            Entity box = AddBox(new Vector3(0f, 0f, 2f), new Vector3(0.25f), BoxKind.Static);
            Span<Entity> hits = stackalloc Entity[2];
            hits.Fill(Entity.None);
            Vector3 halfExtents = new Vector3(3f, 0.5f, 0.5f);

            Assert.That(Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits), Is.Zero);
            Assert.That(Physics.BoxOverlap(Vector3.Zero, halfExtents,
                Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI * 0.5f), hits), Is.EqualTo(1));
            Assert.That(hits[0], Is.EqualTo(box));
            Assert.That(hits[1], Is.EqualTo(Entity.None));
        }

        /// <summary>The overlap filters and capacity match the native query, without writing past its slice.</summary>
        [Test]
        public void BoxOverlapAppliesTheNativeFiltersAndBufferLimit()
        {
            Entity solid = AddBox(new Vector3(-2f, 0f, 0f), Vector3.One, BoxKind.Static);
            Entity trigger = AddBox(Vector3.Zero, Vector3.One, BoxKind.Trigger);
            Entity dynamicBox = AddBox(new Vector3(2f, 0f, 0f), Vector3.One, BoxKind.Dynamic);
            var hits = new Entity[5];
            Array.Fill(hits, Entity.None);
            Vector3 halfExtents = new Vector3(5f);

            int count = Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits.AsSpan(1, 3));
            Assert.That(count, Is.EqualTo(3));
            Assert.That(hits.Skip(1).Take(count), Is.EquivalentTo(new[] { solid, trigger, dynamicBox }));
            Assert.That(hits[0], Is.EqualTo(Entity.None));
            Assert.That(hits[4], Is.EqualTo(Entity.None));
            count = Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits, ignoreTriggers: true);
            Assert.That(count, Is.EqualTo(2));
            Assert.That(hits.Take(count), Is.EquivalentTo(new[] { solid, dynamicBox }));
            count = Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits, layerMask: 1u << 1);
            Assert.That(count, Is.EqualTo(1));
            Assert.That(hits[0], Is.EqualTo(dynamicBox));
            Assert.That(Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits.AsSpan(0, 1)), Is.EqualTo(1));
            Assert.That(Physics.BoxOverlap(Vector3.Zero, halfExtents, Quaternion.Identity, hits, layerMask: 0), Is.Zero);
        }

        /// <summary>An empty buffer, a miss, or an invalid box writes no result.</summary>
        [Test]
        public void BoxOverlapLeavesTheBufferAloneWhenThereAreNoHits()
        {
            AddBox(Vector3.Zero, Vector3.One, BoxKind.Static);
            Span<Entity> hits = stackalloc Entity[1];
            hits[0] = Entity.None;

            Assert.That(Physics.BoxOverlap(Vector3.Zero, Vector3.One, Quaternion.Identity, Span<Entity>.Empty), Is.Zero);
            Assert.That(Physics.BoxOverlap(new Vector3(10f), Vector3.One, Quaternion.Identity, hits), Is.Zero);
            Assert.That(Physics.BoxOverlap(Vector3.Zero, Vector3.Zero, Quaternion.Identity, hits), Is.Zero);
            Assert.That(Physics.BoxOverlap(Vector3.Zero, Vector3.One, default, hits), Is.Zero);
            Assert.That(hits[0], Is.EqualTo(Entity.None));
        }

        /// <summary>Outside play mode both queries return no results and leave the caller's buffers untouched.</summary>
        [Test]
        public void BufferedQueriesReturnNoResultsOutsidePlay()
        {
            Assert.That(GE_TestScene_Close(), Is.EqualTo(0));
            Span<RayHit> rays = stackalloc RayHit[1];
            Span<Entity> overlaps = stackalloc Entity[1];
            rays[0] = new RayHit { Distance = -1f };
            overlaps[0] = Entity.None;

            Assert.That(Physics.RayCastAll(Vector3.Zero, Vector3.UnitZ, rays), Is.Zero);
            Assert.That(Physics.BoxOverlap(Vector3.Zero, Vector3.One, Quaternion.Identity, overlaps), Is.Zero);
            Assert.That(rays[0].Distance, Is.EqualTo(-1f));
            Assert.That(overlaps[0], Is.EqualTo(Entity.None));
        }

        private const int kRayCastCalls = 1_000_000;
        private const int kRayCastRounds = 7;

        private static double TimeRayCast()
        {
            int hits = 0;
            Vector3 forward = Vector3.UnitZ;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            for (int i = 0; i < kRayCastCalls; i++)
                hits += Physics.RayCast(Vector3.Zero, forward, out RayHit _) ? 1 : 0;
            watch.Stop();
            Assert.That(hits, Is.EqualTo(kRayCastCalls));
            return watch.Elapsed.TotalMilliseconds * 1e6 / kRayCastCalls;
        }

        /// <summary>
        /// One million ray casts through the generated binding, each a hit on a box in front of the origin, so every
        /// call runs the full query. Run it from a Release build. Prints nanoseconds per call for each round and the
        /// median.
        /// </summary>
        [Test, Explicit("Micro-benchmark: Physics.RayCast through the generated binding")]
        public void Bench_RayCast()
        {
            AddBox(new Vector3(0f, 0f, 10f), Vector3.One, BoxKind.Static);
            TimeRayCast();
            var rounds = new double[kRayCastRounds];
            for (int round = 0; round < kRayCastRounds; round++)
            {
                rounds[round] = TimeRayCast();
                TestContext.WriteLine($"RayCast round {round}: {rounds[round]:F2} ns/call");
            }
            Array.Sort(rounds);
            TestContext.WriteLine($"RayCast median of {kRayCastRounds} x {kRayCastCalls}: {rounds[kRayCastRounds / 2]:F2} ns/call");
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static WeakReference SubscribeFromCollectibleContext(Entity box, out int subscribed)
        {
            var context = new AssemblyLoadContext("PhysicsBindingTests.Scripts", isCollectible: true);
            Assembly scripts = context.LoadFromAssemblyPath(typeof(PhysicsBindingTests).Assembly.Location);
            Type subscriber = scripts.GetType(typeof(CollectibleSubscriber).FullName!, throwOnError: true)!;
            subscriber.GetMethod(nameof(CollectibleSubscriber.Subscribe))!.Invoke(null, new object[] { box.Id });
            subscribed = ScriptSubscriptionRegistry.Count;
            context.Unload();
            return new WeakReference(context);
        }
    }

    /// <summary>Loaded into a collectible context by the test above, as a hot-reloaded script is.</summary>
    public static class CollectibleSubscriber
    {
        /// <summary>Subscribes a handler of this assembly to the entity's ContactBegin.</summary>
        public static void Subscribe(uint entity) => new Entity(entity).Physics.ContactBegin += OnContact;

        private static void OnContact(in ContactEvent e)
        {
        }
    }
}
