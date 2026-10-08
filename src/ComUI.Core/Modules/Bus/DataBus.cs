using Avalonia.Threading;
using ComUI.Sdk;

namespace ComUI.Core;

/// <summary>IBus 默认实现：每主题保留最新帧，晚订阅者立即收到当前帧。</summary>
public sealed class DataBus : IBus
{
    private sealed class Subscription
    {
        public Delegate Handler = null!;
        public bool UiThread;
    }

    private sealed class Topic
    {
        public object? Latest;
        public Type? LatestType;
        public long Count;
        public DateTime? Last;
        public List<Subscription> Subs { get; } = new();
    }

    private readonly object _gate = new();
    private readonly Dictionary<string, Topic> _topics = new(StringComparer.OrdinalIgnoreCase);

    public void Publish<T>(string topic, T payload)
    {
        IDisposable? stale = null;
        Subscription[] subs;
        lock (_gate)
        {
            var t = GetOrCreate(topic);
            // 替换即释放旧帧：原生载荷（IDisposable）由总线统一回收。
            // 契约：订阅回调内必须同步消费完毕（拷贝/上传），回调返回后不得再引用旧帧。
            if (!ReferenceEquals(t.Latest, payload) && t.Latest is IDisposable d && !ReferenceEquals(d, payload as IDisposable))
                stale = d;
            t.Latest = payload;
            // 记录运行时类型：发布方可能是通过 object 调用的（反射场景）
            t.LatestType = payload?.GetType() ?? typeof(T);
            t.Count++;
            t.Last = DateTime.Now;
            subs = t.Subs.ToArray();
        }
        // 旧帧释放在锁外（Dispose 可能较重；此时新帧已生效，晚订阅不会拿到旧帧）
        stale?.Dispose();
        foreach (var s in subs)
            Deliver(s, payload);
        FramePublished?.Invoke(topic, payload);   // 总线级追踪钩子（锁外；订阅方自行封送线程）
    }

    public IDisposable Subscribe<T>(string topic, Action<T> handler, bool uiThread = false)
    {
        Subscription sub;
        T? retained = default;
        bool hasRetained = false;

        lock (_gate)
        {
            var t = GetOrCreate(topic);
            sub = new Subscription { Handler = handler, UiThread = uiThread };
            t.Subs.Add(sub);

            if (t.Latest is not null && t.LatestType is not null && typeof(T).IsAssignableFrom(t.LatestType))
            {
                retained = (T)t.Latest;
                hasRetained = true;
            }
        }

        if (hasRetained)
            Deliver(sub, retained);

        return new Unsub(() =>
        {
            lock (_gate)
            {
                if (_topics.TryGetValue(topic, out var t))
                    t.Subs.RemoveAll(s => s.Handler == handler);
            }
        });
    }

    /// <summary>总线级发布钩子（锁外触发，可能在任意线程）。</summary>
    public event Action<string, object?>? FramePublished;

    public bool TryGetLatestRaw(string topic, out object? payload)
    {
        lock (_gate)
        {
            if (_topics.TryGetValue(topic, out var t))
            {
                payload = t.Latest;
                return true;
            }
        }
        payload = null;
        return false;
    }

    public bool TryGetLatest<T>(string topic, out T value)
    {
        lock (_gate)
        {
            if (_topics.TryGetValue(topic, out var t) &&
                t.Latest is not null && t.LatestType is not null &&
                typeof(T).IsAssignableFrom(t.LatestType))
            {
                value = (T)t.Latest;
                return true;
            }
        }
        value = default!;
        return false;
    }

    public IReadOnlyList<BusTopicInfo> GetTopics()
    {
        lock (_gate)
        {
            return _topics.Select(kv =>
                    new BusTopicInfo(kv.Key, kv.Value.LatestType?.Name, kv.Value.Count, kv.Value.Last))
                .OrderBy(t => t.Topic, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
    }

    private Topic GetOrCreate(string topic)
    {
        if (!_topics.TryGetValue(topic, out var t))
        {
            t = new Topic();
            _topics[topic] = t;
        }
        return t;
    }

    private static void Deliver<T>(Subscription sub, T payload)
    {
        void Invoke()
        {
            // 订阅时的 T 与发布时的 T 可能不同（反射发布为 object），回退到 DynamicInvoke
            if (sub.Handler is Action<T> typed)
                typed(payload);
            else
                sub.Handler.DynamicInvoke(payload);
        }

        if (sub.UiThread)
            Dispatcher.UIThread.Post(Invoke);
        else
            Invoke();
    }

    private sealed class Unsub(Action dispose) : IDisposable
    {
        private Action? _dispose = dispose;
        public void Dispose() => Interlocked.Exchange(ref _dispose, null)?.Invoke();
    }
}
