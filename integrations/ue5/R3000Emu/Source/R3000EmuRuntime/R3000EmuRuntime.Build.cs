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
            "Core",
            "CoreUObject",
            "Engine",
            "AudioMixer",
            "ProceduralMeshComponent",
            "RHI",
            "RenderCore",
        });

        // Source/src is a symlink to R3000-Emu/src
        string RepoSrc = Path.GetFullPath(Path.Combine(ModuleDirectory, "../src"));
        PublicIncludePaths.Add(RepoSrc);
    }
}

