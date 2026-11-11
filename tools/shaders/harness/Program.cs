// spirv_harness: run Ryujinx's Maxwell shader translator offline on blocks extracted by tools/maxwell/containers.py.
//
//   spirv_harness <code.bin> <ctrl.bin> [-o out.spv] [--glsl]
//       one stage: code block + NVN control section as written by containers.py --extract
//       (--glsl: Vulkan GLSL text instead of SPIR-V, to read what the translator made or compare backends)
//   spirv_harness --batch <extracted dir> --out <dir> [--jobs 2] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env vulkan1.3] [--host desktop|moltenvk]
//       every distinct code block named in any manifest.tsv under <extracted dir>; writes <hash>.spv, results.tsv
//       (one line per block) and summary.txt (counts, exceptions and translator messages grouped by text).
//       The translation runs in a child process (--worker, Batch.cs) that journals each block to <out>/journal.tsv, so a
//       native crash in the translator costs one block (status "crash"), not the run, and a stopped run resumes.
//
// Everything this program learns about the block comes from the control section (see docs/nvn/SHADERS.md):
// +0x708 program start (0x30 graphics, 0x100 compute), +0x6fc/+0x700 constant data size/offset. The stage comes
// from the SPH inside the code (graphics) or from the NVN header magic (0x12345679 = compute).

using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace SpirvHarness
{
    sealed record Job(string Hash, string Group, string CodePath, string CtrlPath, int Stages, string GlslcPath = null);

    sealed class Result
    {
        public string Hash, Group, Stage = "?", Status = "?", Error = "", ValError = "", Stack = "";
        public int Stages, SpvBytes;
        public double Ms;
        public List<string> Messages = new();
        public string Resources = "";
        public string Queries = "";
        public string Reflection = "";
        public List<string> Unsupported = new();
    }

    static partial class Program
    {
        const uint ComputeMagic = 0x12345679;
        static readonly TimeSpan Timeout = TimeSpan.FromSeconds(60);
        static bool NoRetry;   // --no-retry: report attempt 1 only (the translator as Ryujinx runs it)
        static bool Debug;     // --debug-mode: DebugMode from attempt 1 (also reports undecodable opcodes)

        static readonly Dictionary<string, string> GlslcByHash = new();   // --reflect <corpus>: code hash -> GLSLC output

        /// NVN2 (Switch 2) control section: magic 0x98761234, version 201 (docs/AMPERE.md section 3).
        public static bool IsNvn2(byte[] ctrl) => ctrl != null && ctrl.Length >= 0xd8 && BitConverter.ToUInt32(ctrl, 0) == 0x98761234 && BitConverter.ToUInt32(ctrl, 4) == 201;

        static int Main(string[] args)
        {
            NoRetry = args.Contains("--no-retry");
            StaticGpuAccessor.DesktopHost = Opt(args, "--host", "desktop") != "moltenvk";
            StaticGpuAccessor.TexturePoolLength = int.Parse(Opt(args, "--texture-pool", "65536"));
            StaticGpuAccessor.SamplerPoolLength = int.Parse(Opt(args, "--sampler-pool", "4096"));
            Debug = args.Contains("--debug-mode");
            try
            {
                if (args.Length >= 2 && args[0] == "--warp-tests") return WarpTests.Run(args[1]);
                if (args.Length >= 2 && args[0] == "--batch") return Batch(args);
                if (args.Length >= 2 && args[0] == "--worker") return Worker(args);
                if (args.Length >= 2 && !args[0].StartsWith("-")) return Single(args);
            }
            catch (ArgumentException e)
            {
                Console.Error.WriteLine(e.Message);
            }
            Console.Error.WriteLine("usage: spirv_harness <code.bin> <ctrl.bin> [-o out.spv] [--glsl] [--glslc <GLSLC output, NVN2>]\n" +
                                    "       spirv_harness --batch <extracted dir> --out <dir> [--jobs N] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env E] [--host desktop|moltenvk] [--spirv-val PATH] [--reflect <Ampere corpus>] [--crash-budget N]");
            return 2;
        }

        // spirv-val: $VULKAN_SDK\Bin, else the PATH.
        static string FindSpirvVal()
        {
            string exe = OperatingSystem.IsWindows() ? "spirv-val.exe" : "spirv-val";
            string sdk = Environment.GetEnvironmentVariable("VULKAN_SDK");
            if (!string.IsNullOrEmpty(sdk) && File.Exists(Path.Combine(sdk, "Bin", exe))) return Path.Combine(sdk, "Bin", exe);
            return exe;
        }

        static string Opt(string[] args, string name, string def)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
        }

        static int Single(string[] args)
        {
            bool glsl = args.Contains("--glsl");
            string output = Opt(args, "-o", Path.ChangeExtension(args[0], glsl ? ".glsl" : ".spv"));
            string glslcPath = Opt(args, "--glslc", null);
            Result r = Translate(File.ReadAllBytes(args[0]), File.ReadAllBytes(args[1]), out byte[] spv, glsl, glslcPath);
            if (spv != null) File.WriteAllBytes(output, spv);
            Console.WriteLine($"{r.Stage} {r.Status} {r.SpvBytes} bytes {r.Ms:F1} ms -> {(spv != null ? output : "-")}");
            Console.WriteLine($"resources: {r.Resources}");
            foreach (string m in r.Messages) Console.WriteLine($"translator: {m}");
            foreach (string m in r.Unsupported) Console.WriteLine($"unsupported: {m}");
            if (r.Reflection != "") Console.WriteLine($"glslc reflection: {r.Reflection}");
            if (r.Error != "") Console.WriteLine($"error: {r.Error}");
            if (r.Stack != "" && args.Contains("--trace")) Console.WriteLine(r.Stack);
            return spv != null ? 0 : 1;
        }

        // One code block through the translator. Never throws: failures (also a short or malformed control section or
        // code block, read before the translator's own try, and the Ampere SASS decoder) are recorded in the result.
        static Result Translate(byte[] code, byte[] ctrl, out byte[] spv, bool glsl = false, string glslcPath = null)
        {
            try
            {
                return TranslateAny(code, ctrl, out spv, glsl, glslcPath);
            }
            catch (Exception e)
            {
                spv = null;
                return ExceptionResult(e);
            }
        }

        static Result ExceptionResult(Exception e)
        {
            Result r = new() { Status = "exception", Error = $"{e.GetType().Name}: {e.Message}", Stack = e.StackTrace ?? "" };
            string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader") || l.Contains("Nx.Shaders"))?.Trim();
            if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
            return r;
        }

        static Result TranslateAny(byte[] code, byte[] ctrl, out byte[] spv, bool glsl, string glslcPath)
        {
            if (IsNvn2(ctrl)) return TranslateNvn2(code, ctrl, out spv, glsl, glslcPath);
            if (CudaKernel.Is(ctrl)) return TranslateCuda(code, ctrl, out spv, glsl);
            Result r = new();
            spv = null;
            int start = BitConverter.ToInt32(ctrl, 0x708);
            int constSize = BitConverter.ToInt32(ctrl, 0x6fc);
            int constOff = BitConverter.ToInt32(ctrl, 0x700);
            bool compute = BitConverter.ToUInt32(code, 0) == ComputeMagic ||
                           (ctrl.Length >= 0x718 && BitConverter.ToUInt32(ctrl, 0x714) == 5);   // NVN stage 5 (also set by dksh_to_corpus.py)
            // SPH word 0 bits 10..13. Used here for binding ranges only; the translator reads the SPH itself.
            int sphType = compute ? 0 : (BitConverter.ToInt32(code, start) >> 10) & 0xf;
            // SPH types: 0/1 vertex (A / B), 2 tessellation control, 3 tessellation evaluation, 4 geometry, 5 pixel.
            ShaderStage stage = compute ? ShaderStage.Compute : sphType switch
            {
                5 => ShaderStage.Fragment,
                4 => ShaderStage.Geometry,
                2 => ShaderStage.TessellationControl,
                3 => ShaderStage.TessellationEvaluation,
                _ => ShaderStage.Vertex,
            };
            r.Stage = stage.ToString().ToLowerInvariant();

            Stopwatch sw = Stopwatch.StartNew();
            // Attempt 1: normal translation. Attempt 2 (only after an exception, SPIR-V only, unless NoRetry):
            // DebugMode, which turns off the translator's expression propagation. That pass folds the temporaries
            // Ryujinx creates for SPIR-V call arguments back into the call, and the SPIR-V backend then throws in
            // GenerateCall (see docs/nvn/TRANSLATOR.md, "Failures"). Status "ok-retry" = translated by attempt 2.
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl);
                TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) |
                                         (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                try
                {
                    ShaderProgram program = Translator.CreateContext((ulong)start, gpu, options).Translate();
                    spv = glsl ? Encoding.UTF8.GetBytes(program.Code) : program.BinaryCode;
                    if (!glsl && spv != null && program.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                    if (!glsl && spv != null) spv = AddMissingBuiltInCapabilities(spv);
                    r.SpvBytes = spv?.Length ?? 0;
                    r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                    r.Resources = DescribeResources(program.Info);
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    r.Stack = e.StackTrace ?? "";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // One NVN2 (Switch 2, Ampere SM86) block: control section +0x38 program end, +0x3c/+0x40 constant data size/offset,
        // +0x56 stage (0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs). Graphics code starts with a 0x80-byte SPH v4, compute has
        // none. The SASS is decoded by tools/shaders/ampere/decoder and emitted into Ryujinx's IR by the Ampere front end
        // (tools/shaders/ampere/frontend); from there on it is Ryujinx's translator as for Maxwell. A block with any
        // instruction form the front end does not handle gets status "unsupported" and no SPIR-V.
        static Result TranslateNvn2(byte[] code, byte[] ctrl, out byte[] spv, bool glsl, string glslcPath)
        {
            Result r = new();
            spv = null;
            int nvnStage = BitConverter.ToUInt16(ctrl, 0x56);
            int progEnd = BitConverter.ToInt32(ctrl, 0x38);
            int constSize = BitConverter.ToInt32(ctrl, 0x3c), constOff = BitConverter.ToInt32(ctrl, 0x40);
            ShaderStage stage = nvnStage switch
            {
                0 => ShaderStage.Vertex, 1 => ShaderStage.Fragment, 2 => ShaderStage.Geometry,
                3 => ShaderStage.TessellationControl, 4 => ShaderStage.TessellationEvaluation, _ => ShaderStage.Compute,
            };
            bool compute = stage == ShaderStage.Compute;
            r.Stage = stage.ToString().ToLowerInvariant();
            int start = compute ? 0 : 0x80;
            int end = Math.Min(progEnd, code.Length) & ~0xf;
            while (end - 16 >= start && code.AsSpan(end - 16, 16).IndexOfAnyExcept((byte)0) < 0) end -= 16;
            if (nvnStage > 5 || end <= start)
            {
                r.Status = "exception";
                r.Error = $"NVN2 block without code (stage {nvnStage}, program end 0x{progEnd:x}, code 0x{code.Length:x} bytes)";
                return r;
            }
            Nvn2Reflection refl = glslcPath != null && File.Exists(glslcPath) ? Nvn2Reflection.Read(glslcPath) : null;
            if (refl != null) r.Reflection = refl.Describe(nvnStage);

            Stopwatch sw = Stopwatch.StartNew();
            IReadOnlyList<SassInst> insts = SassDecoder.DecodeBlock(code, start, end);
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                // Address 0 of the accessor's code is the SPH (graphics) or the first instruction (compute); the constant
                // data offset (+0x40) is from the start of the code area in both cases.
                StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl) { DrawParametersFromBuiltIns = true };
                if (refl != null) foreach (var h in refl.BufferTextureHandles(nvnStage, ctrl)) gpu.BufferTextures.Add(h);
                else foreach (var h in BufferFetchHandles(insts, compute)) gpu.BufferTextures.Add(h);
                TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) |
                                         (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                AmpereProgram prog = null;
                try
                {
                    TranslatorContext tctx = AmpereTranslator.CreateContext(gpu, options, insts, out prog);
                    ShaderProgram sp = tctx.Translate();
                    if (prog.Unsupported.Count == 0)
                    {
                        spv = glsl ? Encoding.UTF8.GetBytes(sp.Code) : sp.BinaryCode;
                        if (!glsl && spv != null && sp.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                        if (!glsl && spv != null) spv = AddMissingBuiltInCapabilities(spv);
                        r.SpvBytes = spv?.Length ?? 0;
                        r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                        r.Resources = DescribeResources(sp.Info);
                        if (refl != null) lock (gpu.Messages) gpu.Messages.AddRange(refl.Check(nvnStage, sp.Info));
                    }
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    r.Stack = e.StackTrace ?? "";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                if (prog != null && prog.Unsupported.Count > 0)
                {
                    r.Status = "unsupported";
                    r.Unsupported = prog.Unsupported.ToList();
                    r.Error = "unsupported: " + string.Join("; ", r.Unsupported.Take(8)) + (r.Unsupported.Count > 8 ? $"; ... {r.Unsupported.Count - 8} more" : "");
                    r.SpvBytes = 0;
                    spv = null;
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // One CUDA kernel (DLSS, subsdk0 cubins): the .text is SASS with no SPH, bank 0 is CUDA's (block and grid size at
        // c[0][0x0..0x14], parameters from the descriptor's offset, normally 0x160), indirect branch targets come from .nv.info.
        // Same Ampere front end as NVN2, compute only; the work group is a default the runtime overrides with specialization
        // constants 0..2 (AddWorkgroupSizeSpecConstants).
        static Result TranslateCuda(byte[] code, byte[] ctrl, out byte[] spv, bool glsl)
        {
            Result r = new() { Stage = "compute" };
            spv = null;
            CudaKernel k = CudaKernel.Read(ctrl);
            int end = Math.Min(k.TextSize, code.Length) & ~0xf;
            Stopwatch sw = Stopwatch.StartNew();
            IReadOnlyList<SassInst> insts = SassDecoder.DecodeBlock(code, 0, end);
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                StaticGpuAccessor gpu = new(code, 0, 0, ShaderStage.Compute, null);
                (int x, int y, int z) = k.LocalSize;
                gpu.SetCompute(x, y, z, k.Shared);
                TranslationFlags flags = TranslationFlags.Compute | (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                AmpereProgram prog = null;
                try
                {
                    TranslatorContext tctx = AmpereTranslator.CreateContext(gpu, options, insts, out prog, k.IndirectTargets, cuda: true);
                    ShaderProgram sp = tctx.Translate();
                    if (prog.Unsupported.Count == 0)
                    {
                        spv = glsl ? Encoding.UTF8.GetBytes(sp.Code) : sp.BinaryCode;
                        if (!glsl && spv != null && sp.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                        if (!glsl && spv != null) spv = AddWorkgroupSizeSpecConstants(spv);
                        r.SpvBytes = spv?.Length ?? 0;
                        r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                        r.Resources = DescribeResources(sp.Info);
                    }
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    r.Stack = e.StackTrace ?? "";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                if (prog != null && prog.Unsupported.Count > 0)
                {
                    r.Status = "unsupported";
                    r.Unsupported = prog.Unsupported.ToList();
                    r.Error = "unsupported: " + string.Join("; ", r.Unsupported.Take(8)) + (r.Unsupported.Count > 8 ? $"; ... {r.Unsupported.Count - 8} more" : "");
                    r.SpvBytes = 0;
                    spv = null;
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // CUDA kernels run with the block size of each launch, which the cubin does not always fix. Turn the module's
        // LocalSize into the GLSL local_size_x_id = 0..2 form: three OpSpecConstant (default = the LocalSize values) with
        // SpecId 0, 1, 2 and an OpSpecConstantComposite decorated BuiltIn WorkgroupSize, which overrides LocalSize.
        static byte[] AddWorkgroupSizeSpecConstants(byte[] spv)
        {
            const uint OpTypeInt = 21, OpTypeVector = 23, OpExecutionMode = 16, OpDecorate = 71, OpSpecConstant = 50,
                       OpSpecConstantComposite = 51, OpFunction = 54, OpVariable = 59, OpTypePointer = 32;
            const uint ModeLocalSize = 17, DecorationSpecId = 1, DecorationBuiltIn = 11, BuiltInWorkgroupSize = 25;
            uint[] all = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, all, 0, spv.Length);
            List<uint> w = all.ToList();
            uint bound = w[3];
            uint[] local = null;
            uint uintType = 0, uvec3Type = 0;
            int firstDecorate = -1, firstTypeOrConst = -1, firstFunction = -1;
            for (int i = 5; i < w.Count;)
            {
                uint op = w[i] & 0xffff, len = w[i] >> 16;
                if (len == 0) return spv;
                if (op == OpExecutionMode && w[i + 2] == ModeLocalSize) local = [w[i + 3], w[i + 4], w[i + 5]];
                if (op == OpDecorate && firstDecorate < 0) firstDecorate = i;
                if (op == OpDecorate && w[i + 2] == DecorationBuiltIn && w[i + 3] == BuiltInWorkgroupSize) return spv;
                if (op == OpTypeInt && w[i + 2] == 32 && w[i + 3] == 0) uintType = w[i + 1];
                if (op == OpTypeVector && uintType != 0 && w[i + 2] == uintType && w[i + 3] == 3) uvec3Type = w[i + 1];
                if ((op is >= OpTypeInt and <= 39 || op == OpTypePointer || op == OpVariable) && firstTypeOrConst < 0) firstTypeOrConst = i;
                if (op == OpFunction) { firstFunction = i; break; }
                i += (int)len;
            }
            if (local == null || firstFunction < 0) return spv;
            // New types (if missing) and constants go right before the first function: every type they use precedes them.
            List<uint> decl = new();
            if (uintType == 0) { uintType = bound++; decl.AddRange([(4u << 16) | OpTypeInt, uintType, 32, 0]); }
            if (uvec3Type == 0) { uvec3Type = bound++; decl.AddRange([(4u << 16) | OpTypeVector, uvec3Type, uintType, 3]); }
            uint[] ids = [bound++, bound++, bound++];
            uint composite = bound++;
            for (int c = 0; c < 3; c++) decl.AddRange([(4u << 16) | OpSpecConstant, uintType, ids[c], local[c]]);
            decl.AddRange([(6u << 16) | OpSpecConstantComposite, uvec3Type, composite, ids[0], ids[1], ids[2]]);
            List<uint> deco = new();
            for (int c = 0; c < 3; c++) deco.AddRange([(4u << 16) | OpDecorate, ids[c], DecorationSpecId, (uint)c]);
            deco.AddRange([(4u << 16) | OpDecorate, composite, DecorationBuiltIn, BuiltInWorkgroupSize]);
            w.InsertRange(firstFunction, decl);
            int decoAt = firstDecorate >= 0 ? firstDecorate : (firstTypeOrConst >= 0 ? firstTypeOrConst : firstFunction);
            w.InsertRange(decoAt, deco);
            w[3] = bound;
            byte[] result = new byte[w.Count * 4];
            Buffer.BlockCopy(w.ToArray(), 0, result, 0, result.Length);
            return result;
        }

        // Blocks without a GLSLC reflection (the ones only the runtime dumped): a plain 1D TLD (no array, LOD, offset or
        // multisample) is taken as a buffer texture fetch, as Ryujinx does for bindless 1D fetches. In the 99,012 reflected
        // blocks every handle read that way is a buffer sampler (the game has no 1D textures).
        static IEnumerable<(int Slot, int Handle)> BufferFetchHandles(IReadOnlyList<SassInst> insts, bool compute)
        {
            foreach (SassInst i in insts)
            {
                if (i.Opcode != "TLD" || i.Mods.Any(m => m is "LL" or "AOFFI" or "MS")) continue;
                var o = i.Operands;
                if (!o.Any(x => x.Kind == OperandKind.Other && x.Text == "1D")) continue;
                var imms = o.Where(x => x.Kind == OperandKind.Imm).ToList();
                if (imms.Count < 2) continue;
                int bank = (int)imms[0].ImmBits, handle = (int)imms[1].ImmBits;
                if (bank == 2) { yield return (-1, handle); yield return (2, handle); }
                else yield return (bank, handle);
            }
        }

        // Ryujinx declares storage images without a format when the host can load/store them that way (MoltenVK can:
        // shaderStorageImageRead/WriteWithoutFormat), but never adds the two capabilities SPIR-V requires for it.
        // Insert "OpCapability StorageImageReadWithoutFormat / WriteWithoutFormat" right after the 5-word header;
        // capabilities come first in a module, so that is a valid position.
        static byte[] AddImageWithoutFormatCapabilities(byte[] spv)
        {
            const uint OpCapability = 17, ReadWithoutFormat = 55, WriteWithoutFormat = 56;
            uint[] extra = [(2u << 16) | OpCapability, ReadWithoutFormat, (2u << 16) | OpCapability, WriteWithoutFormat];
            byte[] result = new byte[spv.Length + extra.Length * 4];
            Array.Copy(spv, result, 20);
            Buffer.BlockCopy(extra, 0, result, 20, extra.Length * 4);
            Array.Copy(spv, 20, result, 20 + extra.Length * 4, spv.Length - 20);
            return result;
        }

        // Ryujinx's SPIR-V back end declares ViewportIndex as a geometry-shader output (or fragment input) without
        // OpCapability MultiViewport, which spirv-val rejects (docs/debug/001.md). Insert it after the header.
        static byte[] AddMissingBuiltInCapabilities(byte[] spv)
        {
            const uint OpCapability = 17, OpDecorate = 71, DecorationBuiltIn = 11, BuiltInViewportIndex = 10;
            const uint MultiViewport = 57, ShaderViewportIndex = 70, ShaderViewportIndexLayerEXT = 5254;
            uint[] w = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, w, 0, spv.Length);
            bool viewportIndex = false, haveCap = false;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xffff, len = w[i] >> 16;
                if (len == 0) break;
                if (op == OpCapability && (w[i + 1] == MultiViewport || w[i + 1] == ShaderViewportIndex || w[i + 1] == ShaderViewportIndexLayerEXT)) haveCap = true;
                if (op == OpDecorate && len >= 4 && w[i + 2] == DecorationBuiltIn && w[i + 3] == BuiltInViewportIndex) viewportIndex = true;
                i += (int)len;
            }
            if (!viewportIndex || haveCap) return spv;
            uint[] extra = [(2u << 16) | OpCapability, MultiViewport];
            byte[] result = new byte[spv.Length + extra.Length * 4];
            Array.Copy(spv, result, 20);
            Buffer.BlockCopy(extra, 0, result, 20, extra.Length * 4);
            Array.Copy(spv, 20, result, 20 + extra.Length * 4, spv.Length - 20);
            return result;
        }

        // Compact description of what the shader binds, in Maxwell terms and in Vulkan terms:
        //   cb<slot>=s<set>b<binding>            constant buffer c[slot]
        //   sb[c<slot>+0x<off>]=s<set>b<binding> storage buffer whose address the shader loads from c[slot][off]
        //   tex<type>[c<slot>:<handle>]=s<set>b<binding>  texture whose handle is word <handle> of c[slot]
        static string DescribeResources(ShaderProgramInfo info)
        {
            List<string> parts = new();
            foreach (BufferDescriptor b in info.CBuffers) parts.Add($"cb{b.Slot}=s{b.Set}b{b.Binding}");
            foreach (BufferDescriptor b in info.SBuffers) parts.Add($"sb[c{b.SbCbSlot}+0x{b.SbCbOffset:x}]=s{b.Set}b{b.Binding}");
            foreach (TextureDescriptor t in info.Textures)
                parts.Add($"tex{t.Type}[c{t.CbufSlot}:{t.HandleIndex}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            foreach (TextureDescriptor t in info.Images)
                parts.Add($"img{t.Type}[c{t.CbufSlot}:{t.HandleIndex}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            return string.Join(' ', parts).Replace(", ", "|");
        }

        // All distinct code blocks under the extraction root (one manifest.tsv per extracted container).
        static List<Job> LoadJobs(string root)
        {
            Dictionary<string, Job> jobs = new();
            // Ampere corpus layout (tools/shaders/ampere/extract_corpus.py): unique.tsv, code/<h>.bin, ctrl/<h>.bin, glslc/<h>.bin.
            string unique = Path.Combine(root, "unique.tsv");
            if (File.Exists(unique) && Directory.Exists(Path.Combine(root, "code")))
            {
                foreach (var row in ReadTsv(unique))
                {
                    string h = row["code_sha1_16"];
                    jobs[h] = new Job(h, row["stage"], Path.Combine(root, "code", h + ".bin"), Path.Combine(root, "ctrl", row["first_ctrl"] + ".bin"),
                                      int.Parse(row["occurrences"]), Path.Combine(root, "glslc", row["first_glslc"] + ".bin"));
                }
                return jobs.Values.ToList();
            }
            foreach (string manifest in Directory.EnumerateFiles(root, "manifest.tsv", SearchOption.AllDirectories)
                                                .OrderBy(x => x.Replace('\\', '/'), StringComparer.Ordinal))  // same order on Windows
            {
                string dir = Path.GetDirectoryName(manifest);
                string group = Path.GetRelativePath(root, dir);
                foreach (string line in File.ReadLines(manifest).Skip(1))
                {
                    string[] f = line.Split('\t');
                    string hash = f[10];
                    if (jobs.TryGetValue(hash, out Job j)) jobs[hash] = j with { Stages = j.Stages + 1 };
                    else jobs[hash] = new Job(hash, group, Path.Combine(dir, f[11]), Path.Combine(dir, f[12]), 1, GlslcByHash.GetValueOrDefault(hash));
                }
            }
            return jobs.Values.ToList();
        }

        static IEnumerable<Dictionary<string, string>> ReadTsv(string path)
        {
            string[] header = null;
            foreach (string line in File.ReadLines(path))
            {
                string[] f = line.Split('\t');
                if (header == null) { header = f; continue; }
                Dictionary<string, string> row = new();
                for (int k = 0; k < header.Length && k < f.Length; k++) row[header[k]] = f[k];
                yield return row;
            }
        }

        // spirv-val prints "error: line N: <message>" then the offending instruction; keep the first line.
        static string RunSpirvVal(string tool, string targetEnv, string path)
        {
            ProcessStartInfo psi = new(tool, ["--target-env", targetEnv, path])
            {
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            using Process p = Process.Start(psi);
            string err = p.StandardError.ReadToEnd() + p.StandardOutput.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode == 0) return "";
            string first = err.Split('\n').FirstOrDefault(l => l.StartsWith("error")) ?? err.Split('\n')[0];
            return first.Trim();
        }

        static void WriteResults(string path, List<Result> all)
        {
            using StreamWriter w = new(path) { NewLine = "\n" };  // LF on Windows too: same bytes as on macOS
            w.WriteLine("hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\terror\tdraw_time_queries\tglslc_reflection");
            foreach (Result r in all)
            {
                string err = (r.Error + " " + r.ValError).Trim().Replace('\t', ' ').Replace('\n', ' ');
                w.WriteLine($"{r.Hash}\t{r.Group}\t{r.Stage}\t{r.Stages}\t{r.Status}\t{r.Ms:F1}\t{r.SpvBytes}\t{r.Resources}\t" +
                            $"{string.Join(" | ", r.Messages).Replace('\t', ' ')}\t{err}\t{r.Queries}\t{r.Reflection}");
            }
        }

        // Group messages that differ only in numbers ("Attribute offset 0x3A0 ..." etc.) so the summary stays short.
        static string Normalize(string s) => Regex.Replace(Regex.Replace(s, @"0x[0-9A-Fa-f]+", "0x#"), @"\b\d+\b", "#");

        static string Summarize(List<Result> all, TimeSpan elapsed, string targetEnv)
        {
            StringBuilder sb = new();
            int stagesTotal = all.Sum(r => r.Stages);
            sb.AppendLine($"blocks {all.Count} (stages using them: {stagesTotal}), {elapsed.TotalSeconds:F0} s, spirv-val --target-env {targetEnv}");
            sb.AppendLine();
            sb.AppendLine("status        blocks   stages   (per stage kind)");
            foreach (var g in all.GroupBy(r => r.Status).OrderByDescending(g => g.Count()))
            {
                string kinds = string.Join(", ", g.GroupBy(r => r.Stage).OrderBy(k => k.Key).Select(k => $"{k.Key} {k.Count()}"));
                sb.AppendLine($"{g.Key,-12} {g.Count(),7} {g.Sum(r => r.Stages),8}   ({kinds})");
            }
            sb.AppendLine();
            sb.AppendLine("per group (blocks: ok / ok-retry / invalid / exception / timeout / crash)");
            foreach (var g in all.GroupBy(r => Regex.Replace(r.Group, @"^(effect|layout)_.*", "$1_*")).OrderBy(g => g.Key))
            {
                sb.AppendLine($"  {g.Key,-24} {g.Count(),6}: {g.Count(r => r.Status == "ok")} / {g.Count(r => r.Status == "ok-retry")} / {g.Count(r => r.Status == "invalid")} / " +
                              $"{g.Count(r => r.Status == "exception")} / {g.Count(r => r.Status == "timeout")} / {g.Count(r => r.Status == "crash")}");
            }
            void Top(string title, IEnumerable<string> items, int n = 25)
            {
                var groups = items.GroupBy(Normalize).OrderByDescending(g => g.Count()).ToList();
                if (groups.Count == 0) return;
                sb.AppendLine();
                sb.AppendLine(title);
                foreach (var g in groups.Take(n)) sb.AppendLine($"  {g.Count(),6}  {g.First()}");
                if (groups.Count > n) sb.AppendLine($"  ... {groups.Count - n} more kinds");
            }
            Top("exceptions (by message)", all.Where(r => r.Status is "exception" or "timeout" or "crash").Select(r => r.Error));
            // Ampere front end: blocks per unhandled instruction form, and per mnemonic (a block counts once per entry).
            Top("unsupported instruction forms (blocks)", all.SelectMany(r => r.Unsupported), 40);
            Top("unsupported mnemonics (blocks)", all.SelectMany(r => r.Unsupported.Select(u => u.Split(':', '.')[0]).Distinct()), 40);
            Top("spirv-val errors (first error, by message)", all.Where(r => r.ValError != "").Select(r => r.ValError));
            Top("translator messages (blocks reporting them)", all.SelectMany(r => r.Messages));
            Top("draw-time queries (blocks making them)", all.SelectMany(r => r.Queries.Split(',', StringSplitOptions.RemoveEmptyEntries)));
            Top("resource kinds (blocks using them)", all.SelectMany(r => r.Resources.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                .Select(x => Regex.Replace(x, @"=s\d+b\d+$", "")).Distinct()), 60);
            return sb.ToString();
        }
    }
}
