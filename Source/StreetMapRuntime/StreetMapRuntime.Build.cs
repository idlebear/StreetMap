// Copyright 2017 Mike Fricker. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
    public StreetMapRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
		PrivateDependencyModuleNames.AddRange(
			new string[] {
                "Core",
				"CoreUObject",
				"Engine",
				"RHI",
				"RenderCore",
				"ShaderCore",
                "Landscape",
            }
		);
    }
}
