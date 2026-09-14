using System.Reflection;
using System.Text.Json;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Parsers.Kernel;

// ETL読取り専用。採取・権限昇格・シンボル取得は行わない。
if (args.Length == 0 || args.Contains("--help")) {
    Console.WriteLine("etw_extract <input.etl> <output.jsonl>\nETLから絶対QPC、CSwitch、DPC/ISR、thread/imageをJSONLへ抽出。既存出力は上書きしません。");
    return 0;
}
if (args.Length != 2) { Console.Error.WriteLine("引数は input.etl output.jsonl の2つです。"); return 2; }
if (!File.Exists(args[0])) { Console.Error.WriteLine("入力ETLがありません。"); return 2; }
if (File.Exists(args[1])) { Console.Error.WriteLine("既存出力を上書きしません。"); return 2; }

const BindingFlags InstanceFields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
static long LongProperty(Type type, object value, string name) =>
    (long)(type.GetProperty(name, InstanceFields)?.GetValue(value)
        ?? throw new InvalidOperationException($"TraceEventの必要プロパティがありません: {type.Name}.{name}"));
static long SourceField(object value, string name) =>
    (long)(typeof(TraceEventSource).GetField(name, InstanceFields)?.GetValue(value)
        ?? throw new InvalidOperationException($"TraceEventの必要フィールドがありません: {name}"));

using var output = new StreamWriter(new FileStream(args[1], FileMode.CreateNew, FileAccess.Write, FileShare.Read));
void Write(object value) => output.WriteLine(JsonSerializer.Serialize(value));
ETWTraceEventSource? source = null;
var counts = new Dictionary<string,long>();
long? first = null, last = null;
int? headerLost = null, bufferLost = null, clockType = null;
void Count(string type, long qpc) {
    counts[type] = counts.GetValueOrDefault(type) + 1;
    first = first.HasValue ? Math.Min(first.Value,qpc) : qpc;
    last = last.HasValue ? Math.Max(last.Value,qpc) : qpc;
}
void Meta(bool complete, bool final, string? error = null) {
    Write(new { type="meta", final, complete, clock=clockType == 1 ? "qpc" : "unverified",
        clock_type=clockType, qpc_hz=source == null ? (long?)null : LongProperty(typeof(TraceEventSource),source,"QPCFreq"),
        start_qpc=source == null ? (long?)null : SourceField(source,"sessionStartTimeQPC"),
        end_qpc=source == null ? (long?)null : SourceField(source,"sessionEndTimeQPC"),
        lost_events=source == null ? (int?)null : Math.Max(source.EventsLost,headerLost ?? 0), lost_buffers=bufferLost,
        first_event_qpc=first, last_event_qpc=last, counts,
        trace_event_version=typeof(TraceEvent).Assembly.GetName().Version?.ToString(), error });
    output.Flush();
}
try {
    source = new ETWTraceEventSource(Path.GetFullPath(args[0]));
    Meta(false,false);
    var kernel = source.Kernel;
    kernel.EventTraceHeader += e => {
        headerLost = Math.Max(headerLost ?? 0,e.EventsLost);
        bufferLost = Math.Max(bufferLost ?? 0,e.BuffersLost);
        if (clockType.HasValue && clockType != e.ReservedFlags)
            throw new InvalidDataException("複数ETL clock type が不一致です。");
        clockType=e.ReservedFlags;
        Write(new { type="header",qpc=e.TimeStampQPC,clock_type=e.ReservedFlags,qpc_hz=e.PerfFreq,
            lost_events=e.EventsLost,lost_buffers=e.BuffersLost,session=e.SessionName });
    };
    void Thread(ThreadTraceData e) {
        Count("thread",e.TimeStampQPC);
        Write(new { type="thread",qpc=e.TimeStampQPC,cpu=e.ProcessorNumber,pid=e.ProcessID,tid=e.ThreadID,
            @event=e.OpcodeName,name=e.ThreadName,parent_tid=e.ParentThreadID,parent_pid=e.ParentProcessID });
    }
    kernel.ThreadStart += Thread; kernel.ThreadStop += Thread;
    kernel.ThreadDCStart += Thread; kernel.ThreadDCStop += Thread;
    void Image(ImageLoadTraceData e) {
        Count("image",e.TimeStampQPC);
        Write(new { type="image",qpc=e.TimeStampQPC,pid=e.ProcessID,@base=e.ImageBase,size=(uint)e.ImageSize,
            path=e.FileName,@event=e.OpcodeName });
    }
    kernel.ImageLoad += Image; kernel.ImageUnload += Image;
    kernel.ImageDCStart += Image; kernel.ImageDCStop += Image;
    kernel.ThreadCSwitch += e => {
        Count("cswitch",e.TimeStampQPC);
        Write(new { type="cswitch",qpc=e.TimeStampQPC,cpu=e.ProcessorNumber,old_tid=e.OldThreadID,new_tid=e.NewThreadID,
            old_pid=e.OldProcessID,new_pid=e.NewProcessID,wait_reason=e.OldThreadWaitReason.ToString(),
            state=e.OldThreadState.ToString(),old_priority=e.OldThreadPriority,new_priority=e.NewThreadPriority });
    };
    // InitialTimeQPCはイベントpayload先頭のInt64。ElapsedTimeMSecから逆算して丸めない。
    // 対応TraceEventのprivate getterを使用し、API変更時は無言で推定せず失敗する。
    void Dpc(DPCTraceData e) {
        long begin=LongProperty(typeof(DPCTraceData),e,"InitialTimeQPC");
        Count("dpc",e.TimeStampQPC);
        Write(new { type="dpc",begin_qpc=begin,end_qpc=e.TimeStampQPC,cpu=e.ProcessorNumber,
            routine=e.Routine,routine_hex=$"0x{e.Routine:x}",@event=e.OpcodeName });
    }
    kernel.PerfInfoDPC += Dpc; kernel.PerfInfoTimerDPC += Dpc; kernel.PerfInfoThreadedDPC += Dpc;
    kernel.PerfInfoISR += e => {
        long begin=LongProperty(typeof(ISRTraceData),e,"InitialTimeQPC");
        Count("isr",e.TimeStampQPC);
        Write(new { type="isr",begin_qpc=begin,end_qpc=e.TimeStampQPC,cpu=e.ProcessorNumber,
            routine=e.Routine,routine_hex=$"0x{e.Routine:x}",vector=e.Vector,@event=e.OpcodeName });
    };
    source.Process();
    if (clockType != 1) throw new InvalidDataException("QPC時計のETLと確認できません。絶対QPC結合を中止してください。");
    Meta(true,true);
    Console.WriteLine($"抽出完了: {args[1]} / lost events={source.EventsLost}, buffers={bufferLost}");
    return 0;
} catch (Exception error) {
    try { Meta(false,true,error.ToString()); } catch { }
    Console.Error.WriteLine(error);
    return 1;
} finally { source?.Dispose(); }
