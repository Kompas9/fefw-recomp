// --batch: a supervisor process around --worker processes, so that a translator crash the CLR cannot catch (an access
// violation in native code, a stack overflow, a FailFast) costs one block instead of the whole run (GitHub issue #46).
//
//   supervisor (--batch)  owns <out>/journal.tsv's header, starts workers, isolates the blocks a dead worker was on,
//                         and at the end writes results.tsv and summary.txt from the journal.
//   worker (--worker)     the translation itself (same arguments as --batch, plus --only <hash> for one block). Appends
//                         to the journal, flushed per line:
//                             begin<TAB><hash>                 before the block
//                             result<TAB><hash><TAB><json>     the block's Result after its .spv is written
//                         and prints "@done <hash>" per block for the supervisor's progress.
//
// Journal header: "# spirv_harness journal<TAB><TranslatorVersion><TAB><build id><TAB><options>". A header that differs
// (another translator build, other output options) or no journal at all: the journal, every <out>/*.spv and the runtime's
// preload pack (<out>.preload.pack*, built from those files) are deleted and the run starts over. Otherwise blocks with
// a result row are skipped: a run that was stopped or crashed resumes.
//
// A worker that exits non-zero: the blocks it began without finishing are run one at a time (--jobs 1 --only <hash>);
// one that kills that worker too is recorded as status "crash" (no .spv: the game draws without it). The worker then
// restarts on the rest with half the jobs and workstation GC. After --crash-budget crashed blocks (default 50) the run
// stops with an @error line and exit code 1; the journal keeps what was done.
//
// The runtime loads any <hash>.spv it finds without reading results.tsv, so the .spv of a block whose result is not a
// translation (exception, timeout, crash, unsupported, no-output) is deleted. "invalid" (spirv-val, development only)
// keeps its .spv as before.

using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace SpirvHarness
{
    static partial class Program
    {
        // Bump when the output of the translation changes in a way the build id below would not catch.
        const int TranslatorVersion = 2;   // 2: derivative pattern fix (#29)
        const string JournalName = "journal.tsv";
        static readonly JsonSerializerOptions JournalJson = new() { IncludeFields = true };

        sealed class BatchConfig
        {
            public string Root, OutDir, TargetEnv, SpirvVal, Only;
            public int Jobs, Limit, CrashBudget;
            public bool Validate;
            public string JournalPath => Path.Combine(OutDir, JournalName);
        }

        static BatchConfig ReadBatchConfig(string[] args)
        {
            // --reflect <Ampere corpus>: find each block's GLSLC output (reflection section) by code hash.
            string reflect = Opt(args, "--reflect", null);
            if (reflect != null)
                foreach (var row in ReadTsv(Path.Combine(reflect, "unique.tsv")))
                    GlslcByHash[row["code_sha1_16"]] = Path.Combine(reflect, "glslc", row["first_glslc"] + ".bin");
            return new BatchConfig
            {
                Root = args[1],
                OutDir = Opt(args, "--out", null) ?? throw new ArgumentException("--out is required"),
                Jobs = Math.Max(1, int.Parse(Opt(args, "--jobs", "2"))),
                Limit = int.Parse(Opt(args, "--limit", "0")),
                CrashBudget = int.Parse(Opt(args, "--crash-budget", "50")),
                TargetEnv = Opt(args, "--target-env", "vulkan1.3"),
                Validate = !args.Contains("--no-val"),
                SpirvVal = Opt(args, "--spirv-val", FindSpirvVal()),
                Only = Opt(args, "--only", null),
            };
        }

        static List<Job> LoadBatchJobs(BatchConfig c)
        {
            List<Job> jobs = LoadJobs(c.Root);
            if (c.Limit > 0) jobs = jobs.Take(c.Limit).ToList();
            return jobs;
        }

        // What the .spv files depend on besides the blocks: this harness and translator build, and the output options.
        static string JournalHeader(string[] args)
        {
            string build = string.Join(",", new[] { typeof(Program).Assembly, typeof(Translator).Assembly }
                .Distinct().Select(a => a.ManifestModule.ModuleVersionId.ToString("N")[..12]));
            string[] keyed = ["--no-val", "--no-retry", "--debug-mode"];
            string[] valued = ["--target-env", "--host", "--texture-pool", "--sampler-pool", "--reflect"];
            string opts = string.Join(" ", keyed.Where(args.Contains).Concat(valued.Where(v => Opt(args, v, null) != null).Select(v => $"{v}={Opt(args, v, null)}")));
            return $"# spirv_harness journal\t{TranslatorVersion}\t{build}\t{opts}";
        }

        sealed class JournalState
        {
            public Dictionary<string, Result> Results = new();
            public HashSet<string> Begun = new();
            public IEnumerable<string> Unfinished => Begun.Where(h => !Results.ContainsKey(h));
        }

        // Lines that do not parse (a line cut short by a crash) are skipped.
        static JournalState ReadJournal(string path, bool parseResults)
        {
            JournalState s = new();
            if (!File.Exists(path)) return s;
            using FileStream fs = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using StreamReader rd = new(fs, Encoding.UTF8);
            for (string line; (line = rd.ReadLine()) != null;)
            {
                if (line.StartsWith("begin\t")) { s.Begun.Add(line[6..]); continue; }
                if (!line.StartsWith("result\t")) continue;
                int tab = line.IndexOf('\t', 7);
                if (tab < 0) continue;
                string hash = line[7..tab];
                if (!parseResults) { s.Results[hash] = null; continue; }
                try
                {
                    Result r = JsonSerializer.Deserialize<Result>(line[(tab + 1)..], JournalJson);
                    if (r != null) s.Results[hash] = r;
                }
                catch (JsonException) { }
            }
            return s;
        }

        // Appends whole lines, flushed to the OS each time: they survive the process dying right after.
        sealed class JournalWriter : IDisposable
        {
            readonly FileStream fs;
            readonly object gate = new();

            public JournalWriter(string path)
            {
                fs = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                // a line cut short by a crash: start on a new line rather than continue it
                if (fs.Length > 0)
                {
                    using FileStream r = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    r.Seek(-1, SeekOrigin.End);
                    if (r.ReadByte() != '\n') Write("");
                }
            }

            public void Write(string line)
            {
                byte[] b = Encoding.UTF8.GetBytes(line.Replace("\n", " ") + "\n");
                lock (gate)
                {
                    fs.Write(b, 0, b.Length);
                    fs.Flush();
                }
            }

            public void Begin(string hash) => Write($"begin\t{hash}");
            public void Done(Result r) => Write($"result\t{r.Hash}\t{JsonSerializer.Serialize(r, JournalJson)}");
            public void Dispose() => fs.Dispose();
        }

        static void DeleteSpv(string outDir, string hash)
        {
            try { File.Delete(Path.Combine(outDir, hash + ".spv")); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }

        static bool KeepsSpv(string status) => status is "ok" or "ok-retry" or "invalid";

        // ---- worker --------------------------------------------------------------------------------------------------

        static int Worker(string[] args)
        {
            BatchConfig c = ReadBatchConfig(args);
            // The supervisor gone (killed, or its console closed): stop too, rather than translate on unseen. Writes to a
            // closed stdout pipe are ignored on Windows, so this is the only way the worker would notice.
            string parentPid = Opt(args, "--parent", null);
            if (parentPid != null)
            {
                try
                {
                    Process parent = Process.GetProcessById(int.Parse(parentPid));
                    new Thread(() => { parent.WaitForExit(); Environment.Exit(3); }) { IsBackground = true }.Start();
                }
                catch (ArgumentException) { return 3; }   // already gone
            }
            Directory.CreateDirectory(c.OutDir);
            HashSet<string> done = ReadJournal(c.JournalPath, false).Results.Keys.ToHashSet();
            List<Job> jobs = LoadBatchJobs(c).Where(j => !done.Contains(j.Hash) && (c.Only == null || j.Hash == c.Only)).ToList();
            // Test hook (not documented for players): FEFW_HARNESS_TEST_CRASH=<hash>[,<hash>...] kills the worker on those
            // blocks the way a native crash would; "<hash>~" only when it is not alone (--only), i.e. a crash that does not repeat.
            string[] crashOn = (Environment.GetEnvironmentVariable("FEFW_HARNESS_TEST_CRASH") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries);
            using JournalWriter journal = new(c.JournalPath);
            object console = new();
            Parallel.ForEach(jobs, new ParallelOptions { MaxDegreeOfParallelism = c.Jobs }, job =>
            {
                journal.Begin(job.Hash);
                if (crashOn.Contains(job.Hash) || (c.Only == null && crashOn.Contains(job.Hash + "~")))
                    Environment.FailFast($"FEFW_HARNESS_TEST_CRASH: {job.Hash}");
                Result r;
                byte[] spv = null;
                try
                {
                    // A translator bug could loop forever; run on a task so a stuck block is reported, not waited on.
                    Task<(Result, byte[])> t = Task.Run(() =>
                    {
                        Result x = Translate(File.ReadAllBytes(job.CodePath), File.ReadAllBytes(job.CtrlPath), out byte[] s, false, job.GlslcPath);
                        return (x, s);
                    });
                    if (t.Wait(Timeout)) (r, spv) = t.Result;
                    else r = new Result { Status = "timeout", Error = $"no result after {Timeout.TotalSeconds} s" };
                    if (spv != null)
                    {
                        string spvPath = Path.Combine(c.OutDir, job.Hash + ".spv");
                        File.WriteAllBytes(spvPath, spv);
                        if (c.Validate)
                        {
                            r.ValError = RunSpirvVal(c.SpirvVal, c.TargetEnv, spvPath);
                            if (r.ValError != "") r.Status = "invalid";
                        }
                    }
                }
                catch (Exception e)
                {
                    r = ExceptionResult(e is AggregateException a && a.InnerExceptions.Count == 1 ? a.InnerException : e);
                    spv = null;
                }
                if (spv == null || !KeepsSpv(r.Status)) DeleteSpv(c.OutDir, job.Hash);
                r.Hash = job.Hash;
                r.Group = job.Group;
                r.Stages = job.Stages;
                r.Stack = "";
                journal.Done(r);
                lock (console) Console.WriteLine($"@done {job.Hash}");
            });
            return 0;
        }

        // ---- supervisor ----------------------------------------------------------------------------------------------

        static int Batch(string[] args)
        {
            BatchConfig c = ReadBatchConfig(args);
            Directory.CreateDirectory(c.OutDir);
            List<Job> jobs = LoadBatchJobs(c);
            HashSet<string> wanted = jobs.Select(j => j.Hash).ToHashSet();
            Console.WriteLine($"{jobs.Count} distinct code blocks, {c.Jobs} jobs, validation {(c.Validate ? c.TargetEnv : "off")}");

            string header = JournalHeader(args);
            string first = File.Exists(c.JournalPath) ? File.ReadLines(c.JournalPath).FirstOrDefault() : null;
            if (first != header)
            {
                if (first != null) Console.WriteLine("journal.tsv is from another translator build or other options: translating everything again");
                StartOver(c.OutDir);
                File.WriteAllText(c.JournalPath, header + "\n");
            }

            JournalState state = ReadJournal(c.JournalPath, false);
            int Done() => state.Results.Keys.Count(wanted.Contains);
            if (Done() > 0) Console.WriteLine($"{Done()} blocks already translated (journal.tsv): resuming");

            Stopwatch total = Stopwatch.StartNew();
            Progress progress = new(jobs.Count);
            progress.Report(Done(), force: true);
            int jobsN = c.Jobs, stalls = 0, runs = 0;
            // the budget counts crashes of earlier (resumed) runs too
            int crashed = ReadJournal(c.JournalPath, true).Results.Values.Count(r => r.Status == "crash" && wanted.Contains(r.Hash));
            while (Done() < jobs.Count)
            {
                int before = Done();
                int exit = RunWorker(args, jobsN, null, restarted: runs++ > 0, progress, before);
                state = ReadJournal(c.JournalPath, false);
                if (exit == 0 && Done() >= jobs.Count) break;
                List<string> suspects = state.Unfinished.Where(wanted.Contains).ToList();
                Console.WriteLine(exit == 0
                    ? $"translator worker ended with {jobs.Count - Done()} blocks left"
                    : $"translator worker died (exit code {ExitText(exit)}) with {suspects.Count} blocks in progress; trying them one at a time");
                foreach (string h in suspects)
                {
                    int e = RunWorker(args, 1, h, restarted: true, progress, Done());
                    state = ReadJournal(c.JournalPath, false);
                    if (state.Results.ContainsKey(h)) continue;
                    crashed++;
                    Job job = jobs.First(j => j.Hash == h);
                    Result r = new()
                    {
                        Hash = h, Group = job.Group, Stages = job.Stages, Status = "crash",
                        Error = $"the translator process died on this block (exit code {ExitText(e)})",
                    };
                    DeleteSpv(c.OutDir, h);
                    using (JournalWriter w = new(c.JournalPath)) w.Done(r);
                    state.Results[h] = null;
                    Console.WriteLine($"block {h} ({job.Group}) crashed the translator; the game draws without it");
                    progress.Report(Done());
                    if (crashed > c.CrashBudget)
                    {
                        Console.WriteLine($"@error The shader translator crashed on more than {c.CrashBudget} shaders, so Set up stopped translating. " +
                                          $"What is done so far is kept in {Path.GetFullPath(c.JournalPath).Replace('\\', '/')}; Set up continues from there.");
                        return 1;
                    }
                }
                stalls = Done() > before ? 0 : stalls + 1;
                if (stalls >= 3)
                {
                    Console.WriteLine($"@error The shader translator stopped (exit code {ExitText(exit)}) three times without translating anything. " +
                                      "The log above says why.");
                    return 1;
                }
                jobsN = Math.Max(1, jobsN / 2);
            }
            progress.Report(Done(), force: true);

            // results.tsv and summary.txt from the journal (the last row of a block wins)
            List<Result> all = ReadJournal(c.JournalPath, true).Results.Values.Where(r => wanted.Contains(r.Hash)).ToList();
            Dictionary<string, Job> byHash = jobs.ToDictionary(j => j.Hash);
            foreach (Result r in all)
            {
                // a block kept from an earlier run: group and use count from this corpus
                Job j = byHash[r.Hash];
                r.Group = j.Group;
                r.Stages = j.Stages;
                if (!KeepsSpv(r.Status)) DeleteSpv(c.OutDir, r.Hash);
            }
            all = all.OrderBy(r => r.Group).ThenBy(r => r.Hash).ToList();
            WriteResults(Path.Combine(c.OutDir, "results.tsv"), all);
            string summary = Summarize(all, total.Elapsed, c.TargetEnv);
            int crashes = all.Count(r => r.Status == "crash");
            if (crashes > 0) summary = $"crash: {crashes} blocks crashed the translator process; they have no SPIR-V and the game draws without them\n" + summary;
            File.WriteAllText(Path.Combine(c.OutDir, "summary.txt"), summary);
            Console.Write(summary);
            return 0;
        }

        static string ExitText(int code) => code < 0 || code > 0xffff ? $"0x{code:X8}" : code.ToString();

        // A new journal: none of the .spv files or the preload pack the runtime built from them can be trusted.
        static void StartOver(string outDir)
        {
            try { File.Delete(Path.Combine(outDir, JournalName)); } catch (IOException) { }
            foreach (string f in Directory.EnumerateFiles(outDir, "*.spv")) try { File.Delete(f); } catch (IOException) { }
            string full = Path.GetFullPath(outDir).TrimEnd('\\', '/');
            string parent = Path.GetDirectoryName(full);
            if (parent != null && Directory.Exists(parent))
                foreach (string f in Directory.EnumerateFiles(parent, Path.GetFileName(full) + ".preload.pack*")) try { File.Delete(f); } catch (IOException) { }
        }

        // "@progress shaders <done> <total> <text>" (the launcher's Set up shows it) when the percentage moves, or every 5 s.
        sealed class Progress
        {
            readonly int total;
            int lastPct = -1;
            readonly Stopwatch since = Stopwatch.StartNew();
            public Progress(int total) => this.total = total;

            public void Report(int done, bool force = false)
            {
                int pct = total == 0 ? 100 : (int)(done * 100L / total);
                if (!force && pct == lastPct && since.Elapsed.TotalSeconds < 5) return;
                lastPct = pct;
                since.Restart();
                Console.WriteLine($"@progress shaders {done} {total} Translating the shaders: {done} of {total}");
            }
        }

        // One worker process: this program again with --worker and the same arguments. Its stderr goes where ours does;
        // stdout is read for "@done" lines (anything else is passed on).
        static int RunWorker(string[] args, int jobs, string only, bool restarted, Progress progress, int doneBefore)
        {
            List<string> rest = new();
            for (int i = 1; i < args.Length; i++)
            {
                if ((args[i] is "--jobs" or "--only" or "--parent") && i + 1 < args.Length) { i++; continue; }
                rest.Add(args[i]);
            }
            string self = Environment.ProcessPath;
            ProcessStartInfo psi = new(self) { UseShellExecute = false, RedirectStandardOutput = true };
            // under "dotnet spirv_harness.dll" the process is dotnet.exe: name the assembly again
            if (Path.GetFileNameWithoutExtension(self).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
                psi.ArgumentList.Add(typeof(Program).Assembly.Location);
            psi.ArgumentList.Add("--worker");
            foreach (string a in rest) psi.ArgumentList.Add(a);
            psi.ArgumentList.Add("--jobs");
            psi.ArgumentList.Add(jobs.ToString());
            if (only != null) { psi.ArgumentList.Add("--only"); psi.ArgumentList.Add(only); }
            psi.ArgumentList.Add("--parent");
            psi.ArgumentList.Add(Environment.ProcessId.ToString());
            // after a crash: workstation GC (less memory per process); the crash may have been an allocation failure
            if (restarted) psi.Environment["DOTNET_gcServer"] = "0";
            using Process p = Process.Start(psi) ?? throw new InvalidOperationException("could not start the translator worker");
            int done = doneBefore;
            for (string line; (line = p.StandardOutput.ReadLine()) != null;)
            {
                if (line.StartsWith("@done ")) progress.Report(++done);
                else Console.WriteLine(line);
            }
            p.WaitForExit();
            return p.ExitCode;
        }
    }
}
