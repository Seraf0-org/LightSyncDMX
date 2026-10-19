// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncOCIO.h"
#include "OpenColorIOColorSpace.h"
#include "OpenColorIOConfiguration.h"

namespace LightSyncOCIO
{
    bool IsConfigured(const FOpenColorIOColorConversionSettings& Settings)
    {
        return Settings.ConfigurationSource != nullptr && Settings.IsValid();
    }

    FString Describe(const FOpenColorIOColorConversionSettings& Settings)
    {
        if (!Settings.ConfigurationSource)
        {
            return TEXT("(構成アセットなし)");
        }
        return FString::Printf(TEXT("%s: %s → %s"), *Settings.ConfigurationSource->GetName(),
                               *Settings.GetSourceString(), *Settings.GetDestinationString());
    }
}
