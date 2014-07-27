// Copyright � 2014 Mikko Ronkainen <firstname@mikkoronkainen.com>
// License: GPLv3, see the LICENSE file.

#pragma once

#include <QString>

extern "C"
{
#include <stdint.h>
#include "x264/x264.h"
#include "libswscale/swscale.h"
}

namespace OrientView
{
	class VideoDecoder;
	class Settings;
	class FrameData;

	class VideoEncoder
	{

	public:

		VideoEncoder();

		bool initialize(const QString& fileName, VideoDecoder* videoDecoder, Settings* settings);
		void shutdown();

		void loadFrameData(FrameData* frameData);
		void encodeFrame();

	private:

		x264_t* encoder = nullptr;
		x264_picture_t* convertedPicture = nullptr;
		SwsContext* swsContext = nullptr;
	};
}
