// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for the frame-monado XR_FRAME_foveated_projection extension
 * @ingroup external_openxr
 *
 * A projection view's subImage holds a foveated image: per axis, segment i covers pixelCounts[i] image pixels that each
 * stand for |centerSegment - i| + 1 pixels of the view, mapped linearly within the segment. The compositor samples
 * through that mapping instead of the application expanding the image first.
 */
#ifndef XR_FRAME_FOVEATED_PROJECTION_H
#define XR_FRAME_FOVEATED_PROJECTION_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_FRAME_foveated_projection 1
#define XR_FRAME_foveated_projection_SPEC_VERSION 1
#define XR_FRAME_FOVEATED_PROJECTION_EXTENSION_NAME "XR_FRAME_foveated_projection"
#define XR_TYPE_COMPOSITION_LAYER_PROJECTION_FOVEATION_FRAME ((XrStructureType)1000999001)
#define XR_FOVEATION_MAX_SEGMENTS_FRAME 24

typedef struct XrFoveationAxisFRAME
{
	uint32_t centerSegment;
	uint32_t segmentCount;
	uint16_t pixelCounts[XR_FOVEATION_MAX_SEGMENTS_FRAME];
} XrFoveationAxisFRAME;

//! Chained to XrCompositionLayerProjectionView.
typedef struct XrCompositionLayerProjectionFoveationFRAME
{
	XrStructureType type;
	const void *XR_MAY_ALIAS next;
	XrFoveationAxisFRAME x;
	XrFoveationAxisFRAME y;
} XrCompositionLayerProjectionFoveationFRAME;

#ifdef __cplusplus
}
#endif

#endif
