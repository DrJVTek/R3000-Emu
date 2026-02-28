using UnrealBuildTool;
using System.IO;
using System.Diagnostics;

public class R3000EmuRuntime : ModuleRules
{
    public R3000EmuRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnableExceptions = false;
        bUseRTTI = false;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "InputCore",
            "EnhancedInput", "AudioMixer", "ProceduralMeshComponent",
            "RHI", "RenderCore",
        });

        string RepoRoot = FindRepoRoot();
        if (RepoRoot == null)
        {
            System.Console.WriteLine("[R3000] ERROR: Cannot locate R3000-Emu repo root!");
            return;
        }
        System.Console.WriteLine("[R3000] Repo root: {0}", RepoRoot);

        // Headers
        PublicIncludePaths.Add(Path.Combine(RepoRoot, "src"));

        // DLL config
        string LibConfig = (Target.Configuration == UnrealTargetConfiguration.Debug)
            ? "Debug" : "Release";
        string LibDir = Path.Combine(RepoRoot, "lib", LibConfig);

        // Auto-rebuild via CMake
        RebuildCoreLib(RepoRoot, LibConfig);

        // Import library (generated alongside DLL by CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS)
        PublicAdditionalLibraries.Add(Path.Combine(LibDir, "r3000_core.lib"));

        // DLL: delay-load so we can hot-swap without restarting UE5
        PublicDelayLoadDLLs.Add("r3000_core.dll");

        // Copy DLL into plugin Binaries/Win64/ (for editor + packaging)
        string DllSrc = Path.Combine(LibDir, "r3000_core.dll");
        string BinDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../Binaries/Win64"));
        string DllDst = Path.Combine(BinDir, "r3000_core.dll");

        // Physical copy for editor mode (RuntimeDependencies only works for packaging)
        try
        {
            Directory.CreateDirectory(BinDir);
            File.Copy(DllSrc, DllDst, overwrite: true);
            System.Console.WriteLine("[R3000] Copied r3000_core.dll -> {0}", BinDir);
        }
        catch (System.Exception ex)
        {
            System.Console.WriteLine("[R3000] WARNING: Could not copy DLL: {0}", ex.Message);
        }

        RuntimeDependencies.Add(DllDst, DllSrc);

        // Same defines as CMake
        PublicDefinitions.Add("R3000_DBG_LOOP_DETECTORS=1");
    }

    private string FindRepoRoot()
    {
        string srcLink = Path.GetFullPath(Path.Combine(ModuleDirectory, "../src"));
        if (Directory.Exists(srcLink))
        {
            var di = new DirectoryInfo(srcLink);
            var target = di.ResolveLinkTarget(returnFinalTarget: true);
            if (target != null)
                return Path.GetFullPath(Path.Combine(target.FullName, ".."));
        }

        string dir = ModuleDirectory;
        for (int i = 0; i < 10 && dir != null; i++)
        {
            dir = Path.GetDirectoryName(dir);
            if (dir != null && Directory.Exists(Path.Combine(dir, "src", "r3000")))
                return dir;
        }

        return null;
    }

    private void RebuildCoreLib(string RepoRoot, string Config)
    {
        string BuildDir = Path.Combine(RepoRoot, "build");
        if (!Directory.Exists(BuildDir))
        {
            System.Console.WriteLine("[R3000] WARNING: CMake build/ dir not found — run: cmake -B build -G \"Visual Studio 17 2022\" -A x64");
            return;
        }

        var psi = new ProcessStartInfo
        {
            FileName = "cmake",
            Arguments = string.Format("--build \"{0}\" --config {1} --target r3000_core", BuildDir, Config),
            WorkingDirectory = RepoRoot,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        try
        {
            using (var proc = Process.Start(psi))
            {
                string stdout = proc.StandardOutput.ReadToEnd();
                string stderr = proc.StandardError.ReadToEnd();
                proc.WaitForExit(60000);

                if (proc.ExitCode == 0)
                {
                    System.Console.WriteLine("[R3000] r3000_core built ({0})", Config);
                }
                else
                {
                    System.Console.WriteLine("[R3000] WARNING: r3000_core build FAILED:\n{0}\n{1}", stdout, stderr);
                }
            }
        }
        catch (System.Exception ex)
        {
            System.Console.WriteLine("[R3000] WARNING: Could not run cmake: {0}", ex.Message);
        }
    }
}
