#include "StreetMapImporting.h"
#include "StreetMapComponent.h"
#include "StreetMapRailway.h"

#include "LandscapeProxy.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplineControlPoint.h"
#include "ControlPointMeshComponent.h"

#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "StreetMapImporting"

class StreetMapRailwayBuilder
{
private:
	float WorldToCentimeterScale = 100.0f;

public:
	ULandscapeSplinesComponent* ConditionallyCreateSplineComponent(ALandscapeProxy* Landscape, FVector Scale3D)
	{
		Landscape->Modify();

		if (Landscape->SplineComponent == nullptr)
		{
			Landscape->SplineComponent = NewObject<ULandscapeSplinesComponent>(Landscape, NAME_None, RF_Transactional);
			Landscape->SplineComponent->RelativeScale3D = Scale3D;
			Landscape->SplineComponent->AttachToComponent(Landscape->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		}
		Landscape->SplineComponent->ShowSplineEditorMesh(true);

		return Landscape->SplineComponent;
	}

	ULandscapeSplineControlPoint* AddControlPoint(	ULandscapeSplinesComponent* SplinesComponent,
													const FVector& LocalLocation,
													const FStreetMapRailwayBuildSettings& BuildSettings,
													ULandscapeSplineControlPoint* PreviousPoint = nullptr)
	{
		SplinesComponent->Modify();

		ULandscapeSplineControlPoint* NewControlPoint = NewObject<ULandscapeSplineControlPoint>(SplinesComponent, NAME_None, RF_Transactional);
		SplinesComponent->ControlPoints.Add(NewControlPoint);

		// Apply scaling between world and landscape
		FTransform LandscapeToWorld = BuildSettings.Landscape->ActorToWorld();

		NewControlPoint->Location = LocalLocation; // has been scaled before calling this function
		NewControlPoint->Width = BuildSettings.Width;
		NewControlPoint->SideFalloff = 1.5f;
		NewControlPoint->EndFalloff = 3.0f;
		NewControlPoint->LayerName = "Soil";

		if (PreviousPoint)
		{
			NewControlPoint->Rotation = (NewControlPoint->Location - PreviousPoint->Location).Rotation();
			NewControlPoint->Width = PreviousPoint->Width;
			NewControlPoint->SideFalloff = PreviousPoint->SideFalloff;
			NewControlPoint->EndFalloff = PreviousPoint->EndFalloff;
			NewControlPoint->Mesh = PreviousPoint->Mesh;
			NewControlPoint->MeshScale = PreviousPoint->MeshScale;
			NewControlPoint->bPlaceSplineMeshesInStreamingLevels = PreviousPoint->bPlaceSplineMeshesInStreamingLevels;
			NewControlPoint->bEnableCollision = PreviousPoint->bEnableCollision;
			NewControlPoint->bCastShadow = PreviousPoint->bCastShadow;
		}
		else
		{
			// required to make control point visible
			NewControlPoint->UpdateSplinePoints();
		}

		if (!SplinesComponent->IsRegistered())
		{
			SplinesComponent->RegisterComponent();
		}
		else
		{
			SplinesComponent->MarkRenderStateDirty();
		}

		return NewControlPoint;
	}

	ULandscapeSplineSegment* AddSegment(ULandscapeSplineControlPoint* Start, ULandscapeSplineControlPoint* End, bool bAutoRotateStart, bool bAutoRotateEnd)
	{
		ULandscapeSplinesComponent* SplinesComponent = Start->GetOuterULandscapeSplinesComponent();
		SplinesComponent->Modify();
		Start->Modify();
		End->Modify();

		ULandscapeSplineSegment* NewSegment = NewObject<ULandscapeSplineSegment>(SplinesComponent, NAME_None, RF_Transactional);
		SplinesComponent->Segments.Add(NewSegment);

		NewSegment->Connections[0].ControlPoint = Start;
		NewSegment->Connections[1].ControlPoint = End;

		NewSegment->Connections[0].SocketName = Start->GetBestConnectionTo(End->Location);
		NewSegment->Connections[1].SocketName = End->GetBestConnectionTo(Start->Location);

		FVector StartLocation; FRotator StartRotation;
		Start->GetConnectionLocationAndRotation(NewSegment->Connections[0].SocketName, StartLocation, StartRotation);
		FVector EndLocation; FRotator EndRotation;
		End->GetConnectionLocationAndRotation(NewSegment->Connections[1].SocketName, EndLocation, EndRotation);

		// Set up tangent lengths
		NewSegment->Connections[0].TangentLen = (EndLocation - StartLocation).Size();
		NewSegment->Connections[1].TangentLen = NewSegment->Connections[0].TangentLen;

		NewSegment->AutoFlipTangents();

		// set up other segment options
		ULandscapeSplineSegment* CopyFromSegment = nullptr;
		if (Start->ConnectedSegments.Num() > 0)
		{
			CopyFromSegment = Start->ConnectedSegments[0].Segment;
		}
		else if (End->ConnectedSegments.Num() > 0)
		{
			CopyFromSegment = End->ConnectedSegments[0].Segment;
		}
		else
		{
			// Use defaults
		}

		if (CopyFromSegment != nullptr)
		{
			NewSegment->LayerName = CopyFromSegment->LayerName;
			NewSegment->SplineMeshes = CopyFromSegment->SplineMeshes;
			NewSegment->LDMaxDrawDistance = CopyFromSegment->LDMaxDrawDistance;
			NewSegment->bRaiseTerrain = CopyFromSegment->bRaiseTerrain;
			NewSegment->bLowerTerrain = CopyFromSegment->bLowerTerrain;
			NewSegment->bPlaceSplineMeshesInStreamingLevels = CopyFromSegment->bPlaceSplineMeshesInStreamingLevels;
			NewSegment->bEnableCollision = CopyFromSegment->bEnableCollision;
			NewSegment->bCastShadow = CopyFromSegment->bCastShadow;
		}

		Start->ConnectedSegments.Add(FLandscapeSplineConnection(NewSegment, 0));
		End->ConnectedSegments.Add(FLandscapeSplineConnection(NewSegment, 1));

		bool bUpdatedStart = false;
		bool bUpdatedEnd = false;
		if (bAutoRotateStart)
		{
			Start->AutoCalcRotation();
			Start->UpdateSplinePoints();
			bUpdatedStart = true;
		}
		if (bAutoRotateEnd)
		{
			End->AutoCalcRotation();
			End->UpdateSplinePoints();
			bUpdatedEnd = true;
		}

		// Control points' points are currently based on connected segments, so need to be updated.
		if (!bUpdatedStart && Start->Mesh)
		{
			Start->UpdateSplinePoints();
		}
		if (!bUpdatedEnd && End->Mesh)
		{
			End->UpdateSplinePoints();
		}

		// If we've called UpdateSplinePoints on either control point it will already have called UpdateSplinePoints on the new segment
		if (!(bUpdatedStart || bUpdatedEnd))
		{
			NewSegment->UpdateSplinePoints();
		}

		return NewSegment;
	}

	float GetLandscapeElevation(const ALandscapeProxy* Landscape, const FVector2D& Location) const
	{
		UWorld* World = Landscape->GetWorld();
		const FVector RayOrigin(Location,    1000000.0f);
		const FVector RayEndPoint(Location, -1000000.0f);

		static FName TraceTag = FName(TEXT("LandscapeTrace"));
		TArray<FHitResult> Results;
		// Each landscape component has 2 collision shapes, 1 of them is specific to landscape editor
		// Trace only ECC_Visibility channel, so we do hit only Editor specific shape
		World->LineTraceMultiByObjectType(Results, RayOrigin, RayEndPoint, FCollisionObjectQueryParams(ECollisionChannel::ECC_Visibility), FCollisionQueryParams(TraceTag, true));

		bool bHitLandscape = false;
		FVector FinalHitLocation;
		for (const FHitResult& HitResult : Results)
		{
			ULandscapeHeightfieldCollisionComponent* CollisionComponent = Cast<ULandscapeHeightfieldCollisionComponent>(HitResult.Component.Get());
			if (CollisionComponent)
			{
				ALandscapeProxy* HitLandscape = CollisionComponent->GetLandscapeProxy();
				if (HitLandscape)
				{
					FinalHitLocation = HitResult.Location;
					bHitLandscape = true;
					break;
				}
			}
		}

		if (bHitLandscape)
		{
			return FinalHitLocation.Z;
		}

		return 0.0f;
	}

	void CleanOldRailways(ULandscapeSplinesComponent* SplinesComponent, const FStreetMapRailwayBuildSettings& BuildSettings, UWorld* World)
	{
		TArray< ULandscapeSplineControlPoint* > SplineControlPointsToDelete;

		const int32 NumSegments = SplinesComponent->Segments.Num();
		for (int32 SegmentIndex = NumSegments - 1; SegmentIndex >= 0; SegmentIndex--)
		{
			ULandscapeSplineSegment* Segment = SplinesComponent->Segments[SegmentIndex];
			if (Segment->SplineMeshes.Num() == 1 &&
				Segment->SplineMeshes[0].Mesh == BuildSettings.RailwayLineMesh)
			{
				SplineControlPointsToDelete.AddUnique(Segment->Connections[0].ControlPoint);
				SplineControlPointsToDelete.AddUnique(Segment->Connections[1].ControlPoint);

				Segment->DeleteSplinePoints();

				SplinesComponent->Segments.RemoveAtSwap(SegmentIndex);
			}
		}

		SplinesComponent->ControlPoints.RemoveAllSwap([&SplineControlPointsToDelete](ULandscapeSplineControlPoint* OtherControlPoint)
		{
			const bool DeleteThisSplineControlPoint = SplineControlPointsToDelete.Contains(OtherControlPoint);
			if (DeleteThisSplineControlPoint)
			{
				OtherControlPoint->DeleteSplinePoints();
			}
			return DeleteThisSplineControlPoint;
		});

		SplinesComponent->Modify();

		World->ForceGarbageCollection(true);
	}

	void Build(class UStreetMapComponent* StreetMapComponent, const FStreetMapRailwayBuildSettings& BuildSettings)
	{
		const FTransform LandscapeToWorld = BuildSettings.Landscape->ActorToWorld();
		const FVector SplineScaleXYZ = FVector(1.0f) / LandscapeToWorld.GetScale3D();
		ULandscapeSplinesComponent* SplinesComponent = ConditionallyCreateSplineComponent(BuildSettings.Landscape, SplineScaleXYZ);

		CleanOldRailways(SplinesComponent, BuildSettings, BuildSettings.Landscape->GetWorld());

		TMap< int32, ULandscapeSplineControlPoint* > NodeIndexToControlPointMap;
		const TArray<FStreetMapRailway>& Railways = StreetMapComponent->GetStreetMap()->GetRailways();

		for(const FStreetMapRailway& Railway : Railways)
		{
			ULandscapeSplineControlPoint* PreviousPoint = nullptr;
			const int32 NumPoints = Railway.Points.Num();
			for (int32 PointIndex = 0; PointIndex < NumPoints; PointIndex++)
			{
				const FVector2D& PointLocation = Railway.Points[PointIndex];
				const int32 NodeIndex = Railway.NodeIndices[PointIndex];

				ULandscapeSplineControlPoint* CurrentPoint = nullptr;

				// Try to find existing control point first
				if (NodeIndex != INDEX_NONE)
				{
					ULandscapeSplineControlPoint** CurrentPointPtr = NodeIndexToControlPointMap.Find(NodeIndex);
					if (CurrentPointPtr)
					{
						CurrentPoint = *CurrentPointPtr;
					}
				}

				// create a new control point
				if(CurrentPoint == nullptr)
				{
					const float WorldElevation = GetLandscapeElevation(BuildSettings.Landscape, PointLocation);
					const float ScaledWorldElevation = WorldElevation;
					const FVector2D& ScaledPointLocation = PointLocation;

					CurrentPoint = AddControlPoint(SplinesComponent, FVector(ScaledPointLocation, ScaledWorldElevation), BuildSettings, PreviousPoint);

					if (NodeIndex != INDEX_NONE)
					{
						NodeIndexToControlPointMap.Add(NodeIndex, CurrentPoint);
					}
				}

				if (PreviousPoint)
				{
					ULandscapeSplineSegment* NewSegment = AddSegment(PreviousPoint, CurrentPoint, true, true);

					if(PointIndex == 1)
					{
						FLandscapeSplineMeshEntry MeshEntry;
						MeshEntry.Mesh = BuildSettings.RailwayLineMesh;
						MeshEntry.bScaleToWidth = false;
						MeshEntry.ForwardAxis = BuildSettings.ForwardAxis;
						MeshEntry.UpAxis = BuildSettings.UpAxis;
						MeshEntry.Scale = FVector(1.0f);

						NewSegment->LayerName = "soil";
						NewSegment->LDMaxDrawDistance = 100000.0f;
						NewSegment->bRaiseTerrain = true;
						NewSegment->bLowerTerrain = true;
						NewSegment->bPlaceSplineMeshesInStreamingLevels = true;
						NewSegment->bEnableCollision = false;
						NewSegment->bCastShadow = true;

						if (NewSegment->SplineMeshes.Num() == 0)
						{
							NewSegment->SplineMeshes.Add(MeshEntry);
						}
					}
				}
				PreviousPoint = CurrentPoint;
			}
		}
	}
};

void BuildRailway(class UStreetMapComponent* StreetMapComponent, const FStreetMapRailwayBuildSettings& BuildSettings)
{
	FScopedTransaction Transaction(LOCTEXT("Undo", "Creating Railways"));

	StreetMapRailwayBuilder Builder;

	Builder.Build(StreetMapComponent, BuildSettings);
}
