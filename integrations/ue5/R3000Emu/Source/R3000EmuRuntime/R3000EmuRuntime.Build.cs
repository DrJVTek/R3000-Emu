using UnrealBuildTool;
using System.IO;

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

        // Emulator sources are symlinked into Private/src/ and compiled by UBT directly.
        // No DLL, no cmake — UBT discovers all .cpp recursively under Private/.
        string RepoRoot = FindRepoRoot();
        if (RepoRoot == null)
        {
            System.Console.WriteLine("[R3000] ERROR: Cannot locate R3000-Emu repo root!");
            return;
        }
        System.Console.WriteLine("[R3000] Repo root: {0}", RepoRoot);

        // Include paths: use the junction-relative path so that #pragma once
        // sees the same file identity as UBT's compilation path (avoids redefinition
        // when the repo root resolves to a different absolute path than the junction).
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "src"));

        // Same defines as CMake
        PublicDefinitions.Add("R3000_DBG_LOOP_DETECTORS=1");
    }

    private string FindRepoRoot()
    {
        // The symlink is now at Private/src → repo src/
        // Resolve it to find the repo root (parent of src/)
        string srcLink = Path.GetFullPath(Path.Combine(ModuleDirectory, "Private/src"));
        if (Directory.Exists(srcLink))
        {
            var di = new DirectoryInfo(srcLink);
            var target = di.ResolveLinkTarget(returnFinalTarget: true);
            if (target != null)
                return Path.GetFullPath(Path.Combine(target.FullName, ".."));
        }

        // Fallback: walk up looking for src/r3000/
        string dir = ModuleDirectory;
        for (int i = 0; i < 10 && dir != null; i++)
        {
            dir = Path.GetDirectoryName(dir);
            if (dir != null && Directory.Exists(Path.Combine(dir, "src", "r3000")))
                return dir;
        }

        return null;
    }
}
