// Copyright � 2014 Mikko Ronkainen <firstname@mikkoronkainen.com>
// License: GPLv3, see the LICENSE file.

#include <QOpenGLPixelTransferOptions>

#include "Renderer.h"
#include "VideoDecoder.h"
#include "QuickRouteReader.h"
#include "MapImageReader.h"
#include "VideoStabilizer.h"
#include "InputHandler.h"
#include "Settings.h"
#include "FrameData.h"

using namespace OrientView;

bool Renderer::initialize(VideoDecoder* videoDecoder, QuickRouteReader* quickRouteReader, MapImageReader* mapImageReader, VideoStabilizer* videoStabilizer, InputHandler* inputHandler, Settings* settings)
{
	qDebug("Initializing the renderer");

	this->videoStabilizer = videoStabilizer;
	this->inputHandler = inputHandler;

	videoPanel.textureWidth = videoDecoder->getFrameWidth();
	videoPanel.textureHeight = videoDecoder->getFrameHeight();
	videoPanel.texelWidth = 1.0 / videoPanel.textureWidth;
	videoPanel.texelHeight = 1.0 / videoPanel.textureHeight;
	videoPanel.userScale = settings->appearance.videoPanelScale;
	videoPanel.clearColor = settings->appearance.videoPanelBackgroundColor;
	videoPanel.clearEnabled = !settings->stabilizer.disableVideoClear;

	mapPanel.textureWidth = mapImageReader->getMapImage().width();
	mapPanel.textureHeight = mapImageReader->getMapImage().height();
	mapPanel.texelWidth = 1.0 / mapPanel.textureWidth;
	mapPanel.texelHeight = 1.0 / mapPanel.textureHeight;
	mapPanel.clearColor = settings->appearance.mapPanelBackgroundColor;
	mapPanel.relativeWidth = settings->appearance.mapPanelWidth;

	showInfoPanel = settings->appearance.showInfoPanel;
	multisamples = settings->window.multisamples;

	const double movingAverageAlpha = 0.1;
	averageFps.reset();
	averageFps.setAlpha(movingAverageAlpha);
	averageFrameTime.reset();
	averageFrameTime.setAlpha(movingAverageAlpha);
	averageDecodeTime.reset();
	averageDecodeTime.setAlpha(movingAverageAlpha);
	averageStabilizeTime.reset();
	averageStabilizeTime.setAlpha(movingAverageAlpha);
	averageRenderTime.reset();
	averageRenderTime.setAlpha(movingAverageAlpha);
	averageEncodeTime.reset();
	averageEncodeTime.setAlpha(movingAverageAlpha);
	averageSpareTime.reset();
	averageSpareTime.setAlpha(movingAverageAlpha);

	initializeOpenGLFunctions();

	if (!resizeWindow(settings->window.width, settings->window.height))
		return false;

	if (!loadShaders(&videoPanel, settings->shaders.videoPanelShader))
		return false;

	if (!loadShaders(&mapPanel, settings->shaders.mapPanelShader))
		return false;

	// 1 2
	// 4 3
	GLfloat videoPanelBuffer[] =
	{
		-(float)videoPanel.textureWidth / 2, (float)videoPanel.textureHeight / 2, 0.0f, // 1
		(float)videoPanel.textureWidth / 2, (float)videoPanel.textureHeight / 2, 0.0f, // 2
		(float)videoPanel.textureWidth / 2, -(float)videoPanel.textureHeight / 2, 0.0f, // 3
		-(float)videoPanel.textureWidth / 2, -(float)videoPanel.textureHeight / 2, 0.0f, // 4

		0.0f, 0.0f, // 1
		1.0f, 0.0f, // 2
		1.0f, 1.0f, // 3
		0.0f, 1.0f  // 4
	};

	// 1 2
	// 4 3
	GLfloat mapPanelBuffer[] =
	{
		-(float)mapPanel.textureWidth / 2, (float)mapPanel.textureHeight / 2, 0.0f, // 1
		(float)mapPanel.textureWidth / 2, (float)mapPanel.textureHeight / 2, 0.0f, // 2
		(float)mapPanel.textureWidth / 2, -(float)mapPanel.textureHeight / 2, 0.0f, // 3
		-(float)mapPanel.textureWidth / 2, -(float)mapPanel.textureHeight / 2, 0.0f, // 4

		0.0f, 0.0f, // 1
		1.0f, 0.0f, // 2
		1.0f, 1.0f, // 3
		0.0f, 1.0f  // 4
	};

	loadBuffer(&videoPanel, videoPanelBuffer, 20);
	loadBuffer(&mapPanel, mapPanelBuffer, 20);

	videoPanel.texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
	videoPanel.texture->create();
	videoPanel.texture->bind();
	videoPanel.texture->setSize(videoPanel.textureWidth, videoPanel.textureHeight);
	videoPanel.texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
	videoPanel.texture->setMinificationFilter(QOpenGLTexture::Linear);
	videoPanel.texture->setMagnificationFilter(QOpenGLTexture::Linear);
	videoPanel.texture->setWrapMode(QOpenGLTexture::ClampToEdge);
	videoPanel.texture->allocateStorage();
	videoPanel.texture->release();

	mapPanel.texture = new QOpenGLTexture(mapImageReader->getMapImage());
	mapPanel.texture->bind();
	mapPanel.texture->setMinificationFilter(QOpenGLTexture::Linear);
	mapPanel.texture->setMagnificationFilter(QOpenGLTexture::Linear);
	mapPanel.texture->setWrapMode(QOpenGLTexture::ClampToEdge);
	mapPanel.texture->release();

	paintDevice = new QOpenGLPaintDevice();
	painter = new QPainter();
	painter->begin(paintDevice);
	painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform | QPainter::HighQualityAntialiasing);
	painter->end();

	routePath = new QPainterPath();

	int routePointCount = quickRouteReader->getRouteData().routePoints.size();

	if (routePointCount >= 2)
	{
		for (int i = 0; i < routePointCount; ++i)
		{
			RoutePoint rp = quickRouteReader->getRouteData().routePoints.at(i);

			double x = rp.position.x();
			double y = -rp.position.y();

			if (i == 0)
				routePath->moveTo(x, y);
			else
				routePath->lineTo(x, y);
		}
	}

	return true;
}

bool Renderer::resizeWindow(int newWidth, int newHeight)
{
	windowWidth = newWidth;
	windowHeight = newHeight;

	fullClearRequested = true;

	QOpenGLFramebufferObjectFormat format;
	format.setSamples(multisamples);
	format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);

	if (outputFramebuffer != nullptr)
	{
		delete outputFramebuffer;
		outputFramebuffer = nullptr;
	}

	outputFramebuffer = new QOpenGLFramebufferObject(windowWidth, windowHeight, format);

	if (!outputFramebuffer->isValid())
	{
		qWarning("Could not create main frame buffer");
		return false;
	}

	format.setSamples(0);

	if (outputFramebufferNonMultisample != nullptr)
	{
		delete outputFramebufferNonMultisample;
		outputFramebufferNonMultisample = nullptr;
	}

	outputFramebufferNonMultisample = new QOpenGLFramebufferObject(windowWidth, windowHeight, format);

	if (!outputFramebufferNonMultisample->isValid())
	{
		qWarning("Could not create non multisampled main frame buffer");
		return false;
	}

	if (renderedFrameData.data != nullptr)
	{
		delete renderedFrameData.data;
		renderedFrameData.data = nullptr;
	}

	renderedFrameData = FrameData();
	renderedFrameData.dataLength = (size_t)(windowWidth * windowHeight * 4);
	renderedFrameData.rowLength = (size_t)(windowWidth * 4);
	renderedFrameData.data = new uint8_t[renderedFrameData.dataLength];
	renderedFrameData.width = windowWidth;
	renderedFrameData.height = windowHeight;

	return true;
}

Renderer::~Renderer()
{
	if (renderedFrameData.data != nullptr)
	{
		delete renderedFrameData.data;
		renderedFrameData.data = nullptr;
	}

	if (outputFramebufferNonMultisample != nullptr)
	{
		delete outputFramebufferNonMultisample;
		outputFramebufferNonMultisample = nullptr;
	}

	if (outputFramebuffer != nullptr)
	{
		delete outputFramebuffer;
		outputFramebuffer = nullptr;
	}

	if (routePath != nullptr)
	{
		delete routePath;
		routePath = nullptr;
	}

	if (painter != nullptr)
	{
		delete painter;
		painter = nullptr;
	}

	if (paintDevice != nullptr)
	{
		delete paintDevice;
		paintDevice = nullptr;
	}

	if (mapPanel.texture != nullptr)
	{
		delete mapPanel.texture;
		mapPanel.texture = nullptr;
	}

	if (videoPanel.texture != nullptr)
	{
		delete videoPanel.texture;
		videoPanel.texture = nullptr;
	}

	if (mapPanel.buffer != nullptr)
	{
		delete mapPanel.buffer;
		mapPanel.buffer = nullptr;
	}

	if (videoPanel.buffer != nullptr)
	{
		delete videoPanel.buffer;
		videoPanel.buffer = nullptr;
	}

	if (mapPanel.program != nullptr)
	{
		delete mapPanel.program;
		mapPanel.program = nullptr;
	}

	if (videoPanel.program != nullptr)
	{
		delete videoPanel.program;
		videoPanel.program = nullptr;
	}
}

bool Renderer::loadShaders(Panel* panel, const QString& shaderName)
{
	panel->program = new QOpenGLShaderProgram();

	if (!panel->program->addShaderFromSourceFile(QOpenGLShader::Vertex, QString("data/shaders/%1.vert").arg(shaderName)))
		return false;

	if (!panel->program->addShaderFromSourceFile(QOpenGLShader::Fragment, QString("data/shaders/%1.frag").arg(shaderName)))
		return false;

	if (!panel->program->link())
		return false;

	if ((panel->vertexMatrixUniform = panel->program->uniformLocation("vertexMatrix")) == -1)
		qWarning("Could not find vertexMatrix uniform");

	if ((panel->vertexPositionAttribute = panel->program->attributeLocation("vertexPosition")) == -1)
		qWarning("Could not find vertexPosition attribute");

	if ((panel->vertexTextureCoordinateAttribute = panel->program->attributeLocation("vertexTextureCoordinate")) == -1)
		qWarning("Could not find vertexTextureCoordinate attribute");

	if ((panel->textureSamplerUniform = panel->program->uniformLocation("textureSampler")) == -1)
		qWarning("Could not find textureSampler uniform");

	panel->textureWidthUniform = panel->program->uniformLocation("textureWidth");
	panel->textureHeightUniform = panel->program->uniformLocation("textureHeight");
	panel->texelWidthUniform = panel->program->uniformLocation("texelWidth");
	panel->texelHeightUniform = panel->program->uniformLocation("texelHeight");

	return true;
}

void Renderer::loadBuffer(Panel* panel, GLfloat* buffer, size_t size)
{
	panel->buffer = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
	panel->buffer->setUsagePattern(QOpenGLBuffer::StaticDraw);
	panel->buffer->create();
	panel->buffer->bind();
	panel->buffer->allocate(buffer, (int)(sizeof(GLfloat) * size));
	panel->buffer->release();
}

void Renderer::startRendering(double currentTime, double frameTime, double spareTime, double decoderTime, double stabilizerTime, double encoderTime)
{
	renderTimer.restart();

	this->currentTime = currentTime;
	this->frameTime = frameTime;

	averageFps.addMeasurement(1000.0 / frameTime);
	averageFrameTime.addMeasurement(frameTime);
	averageDecodeTime.addMeasurement(decoderTime);
	averageStabilizeTime.addMeasurement(stabilizerTime);
	averageRenderTime.addMeasurement(lastRenderTime);
	averageEncodeTime.addMeasurement(encoderTime);
	averageSpareTime.addMeasurement(spareTime);

	paintDevice->setSize(QSize(windowWidth, windowHeight));
	glViewport(0, 0, windowWidth, windowHeight);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Renderer::uploadFrameData(FrameData* frameData)
{
	QOpenGLPixelTransferOptions options;

	options.setRowLength((int)(frameData->rowLength / 4));
	options.setImageHeight(frameData->height);
	options.setAlignment(1);

	videoPanel.texture->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, frameData->data, &options);
}

void Renderer::renderAll()
{
	if (isEncoding)
		outputFramebuffer->bind();

	if (renderMode == RenderMode::ALL || renderMode == RenderMode::VIDEO)
		renderVideoPanel();

	if (renderMode == RenderMode::ALL || renderMode == RenderMode::MAP)
		renderMapPanel();

	if (showInfoPanel)
		renderInfoPanel();

	if (isEncoding)
		outputFramebuffer->release();
}

void Renderer::stopRendering()
{
	lastRenderTime = renderTimer.nsecsElapsed() / 1000000.0;
}

void Renderer::getRenderedFrame(FrameData* frameData)
{
	QOpenGLFramebufferObject* sourceFbo = outputFramebuffer;

	// pixels cannot be directly read from a multisampled framebuffer
	// copy the framebuffer to a non-multisampled framebuffer and continue
	if (sourceFbo->format().samples() != 0)
	{
		QRect rect(0, 0, windowWidth, windowHeight);
		QOpenGLFramebufferObject::blitFramebuffer(outputFramebufferNonMultisample, rect, sourceFbo, rect);
		sourceFbo = outputFramebufferNonMultisample;
	}

	sourceFbo->bind();
	glReadPixels(0, 0, windowWidth, windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, renderedFrameData.data);
	sourceFbo->release();

	*frameData = renderedFrameData;
}

void Renderer::renderVideoPanel()
{
	videoPanel.vertexMatrix.setToIdentity();

	if (!shouldFlipOutput)
		videoPanel.vertexMatrix.ortho(-windowWidth / 2, windowWidth / 2, -windowHeight / 2, windowHeight / 2, 0.0f, 1.0f);
	else
		videoPanel.vertexMatrix.ortho(-windowWidth / 2, windowWidth / 2, windowHeight / 2, -windowHeight / 2, 0.0f, 1.0f);

	double videoPanelOffsetX = 0.0;

	if (renderMode != RenderMode::VIDEO)
	{
		videoPanelOffsetX += (windowWidth / 2.0) - (((1.0 - mapPanel.relativeWidth) * windowWidth) / 2.0);
		videoPanel.scale = ((1.0 - mapPanel.relativeWidth) * windowWidth) / videoPanel.textureWidth;
	}
	else
		videoPanel.scale = windowWidth / videoPanel.textureWidth;

	if (videoPanel.scale * videoPanel.textureHeight > windowHeight)
		videoPanel.scale = windowHeight / videoPanel.textureHeight;

	videoPanel.scale *= videoPanel.userScale;

	videoPanel.vertexMatrix.translate(
		videoPanelOffsetX + videoPanel.x + videoPanel.userX + videoStabilizer->getX() * videoPanel.textureWidth * videoPanel.scale,
		videoPanel.y + videoPanel.userY - videoStabilizer->getY() * videoPanel.textureHeight * videoPanel.scale,
		0.0f);

	videoPanel.vertexMatrix.rotate(videoPanel.angle + videoPanel.userAngle - videoStabilizer->getAngle(), 0.0f, 0.0f, 1.0f);
	videoPanel.vertexMatrix.scale(videoPanel.scale);

	if (fullClearRequested)
	{
		glClearColor(videoPanel.clearColor.redF(), videoPanel.clearColor.greenF(), videoPanel.clearColor.blueF(), 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		fullClearRequested = false;
	}

	if (videoPanel.clippingEnabled)
	{
		double videoPanelWidth = videoPanel.scale * videoPanel.textureWidth;
		double videoPanelHeight = videoPanel.scale * videoPanel.textureHeight;
		double leftMargin = (windowWidth - videoPanelWidth) / 2.0;
		double bottomMargin = (windowHeight - videoPanelHeight) / 2.0;

		glEnable(GL_SCISSOR_TEST);
		glScissor((int)(leftMargin + videoPanelOffsetX + videoPanel.x + videoPanel.userX + 0.5),
			(int)(bottomMargin + videoPanel.y + videoPanel.userY + 0.5),
			(int)(videoPanelWidth + 0.5),
			(int)(videoPanelHeight + 0.5));
	}

	if (videoPanel.clearEnabled)
	{
		glClearColor(videoPanel.clearColor.redF(), videoPanel.clearColor.greenF(), videoPanel.clearColor.blueF(), 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	renderPanel(&videoPanel);
	glDisable(GL_SCISSOR_TEST);
}

void Renderer::renderMapPanel()
{
	mapPanel.vertexMatrix.setToIdentity();

	if (!shouldFlipOutput)
		mapPanel.vertexMatrix.ortho(-windowWidth / 2, windowWidth / 2, -windowHeight / 2, windowHeight / 2, 0.0f, 1.0f);
	else
		mapPanel.vertexMatrix.ortho(-windowWidth / 2, windowWidth / 2, windowHeight / 2, -windowHeight / 2, 0.0f, 1.0f);

	mapPanel.scale = windowWidth / mapPanel.textureWidth;

	if (mapPanel.scale * mapPanel.textureHeight > windowHeight)
		mapPanel.scale = windowHeight / mapPanel.textureHeight;

	mapPanel.scale *= mapPanel.userScale;

	mapPanel.vertexMatrix.translate(mapPanel.x + mapPanel.userX, mapPanel.y + mapPanel.userY);
	mapPanel.vertexMatrix.rotate(mapPanel.angle + mapPanel.userAngle, 0.0f, 0.0f, 1.0f);
	mapPanel.vertexMatrix.scale(mapPanel.scale);

	mapPanel.clippingEnabled = (renderMode == RenderMode::ALL);

	int mapBorderX = (int)(mapPanel.relativeWidth * windowWidth + 0.5);

	if (fullClearRequested)
	{
		glClearColor(mapPanel.clearColor.redF(), mapPanel.clearColor.greenF(), mapPanel.clearColor.blueF(), 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		fullClearRequested = false;
	}

	if (mapPanel.clippingEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, mapBorderX, (int)windowHeight);
	}

	if (mapPanel.clearEnabled)
	{
		glClearColor(mapPanel.clearColor.redF(), mapPanel.clearColor.greenF(), mapPanel.clearColor.blueF(), 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	renderPanel(&mapPanel);
	glDisable(GL_SCISSOR_TEST);

	renderRoute();

	if (mapPanel.clippingEnabled)
	{
		painter->begin(paintDevice);
		painter->setPen(QColor(0, 0, 0));
		painter->drawLine(mapBorderX, 0, mapBorderX, (int)windowHeight);
		painter->end();
	}
}

void Renderer::renderInfoPanel()
{
	QFont font = QFont("DejaVu Sans", 8, QFont::Bold);
	QFontMetrics metrics(font);

	int textX = 10;
	int textY = 6;
	int lineHeight = metrics.height();
	int lineSpacing = metrics.lineSpacing() + 1;
	int lineWidth1 = metrics.boundingRect("video scale:").width();
	int lineWidth2 = metrics.boundingRect("99:99:99.999").width();
	int rightPartMargin = 15;
	int backgroundRadius = 10;
	int backgroundWidth = textX + backgroundRadius + lineWidth1 + rightPartMargin + lineWidth2 + 10;
	int backgroundHeight = lineSpacing * 15 + textY + 3;

	QColor textColor = QColor(255, 255, 255, 200);
	QColor textGreenColor = QColor(0, 255, 0, 200);
	QColor textRedColor = QColor(255, 0, 0, 200);

	painter->begin(paintDevice);
	painter->setPen(QColor(0, 0, 0));
	painter->setBrush(QBrush(QColor(20, 20, 20, 220)));
	painter->drawRoundedRect(-backgroundRadius, -backgroundRadius, backgroundWidth, backgroundHeight, backgroundRadius, backgroundRadius);

	painter->setPen(textColor);
	painter->setFont(font);

	painter->drawText(textX, textY, lineWidth1, lineHeight, 0, "time:");

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "fps:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "frame:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "decode:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "stabilize:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "render:");

	if (isEncoding)
		painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "encode:");
	else
		painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "spare:");

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "edit:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "render:");

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "video scale:");
	painter->drawText(textX, textY += lineSpacing, lineWidth1, lineHeight, 0, "map scale:");

	textX += lineWidth1 + rightPartMargin;
	textY = 6;

	QTime currentTimeTemp = QTime(0, 0, 0, 0).addMSecs((int)(currentTime * 1000.0 + 0.5));
	painter->drawText(textX, textY, lineWidth2, lineHeight, 0, currentTimeTemp.toString("HH:mm:ss.zzz"));

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString::number(averageFps.getAverage(), 'f', 2));
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageFrameTime.getAverage(), 'f', 2)));
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageDecodeTime.getAverage(), 'f', 2)));
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageStabilizeTime.getAverage(), 'f', 2)));
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageRenderTime.getAverage(), 'f', 2)));

	if (isEncoding)
		painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageEncodeTime.getAverage(), 'f', 2)));
	else
	{
		if (averageSpareTime.getAverage() < 0)
			painter->setPen(textRedColor);
		else if (averageSpareTime.getAverage() > 0)
			painter->setPen(textGreenColor);

		painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString("%1 ms").arg(QString::number(averageSpareTime.getAverage(), 'f', 2)));
		painter->setPen(textColor);
	}

	QString selectedText;
	QString renderText;

	switch (inputHandler->getEditMode())
	{
		case EditMode::NONE: selectedText = "none"; break;
		case EditMode::VIDEO: selectedText = "video"; break;
		case EditMode::MAP: selectedText = "map"; break;
		case EditMode::MAP_WIDTH: selectedText = "map width"; break;
		default: selectedText = "unknown"; break;
	}

	switch (renderMode)
	{
		case RenderMode::ALL: renderText = "both"; break;
		case RenderMode::VIDEO: renderText = "video"; break;
		case RenderMode::MAP: renderText = "map"; break;
		default: renderText = "unknown"; break;
	}

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, selectedText);
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, renderText);

	textY += lineSpacing;

	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString::number(videoPanel.userScale, 'f', 2));
	painter->drawText(textX, textY += lineSpacing, lineWidth2, lineHeight, 0, QString::number(mapPanel.userScale, 'f', 2));

	painter->end();
}

void Renderer::renderRoute()
{
	QPen pen;
	pen.setColor(QColor(200, 0, 0, 128));
	pen.setWidth(15);
	pen.setCapStyle(Qt::PenCapStyle::RoundCap);
	pen.setJoinStyle(Qt::PenJoinStyle::RoundJoin);

	QMatrix m;
	m.translate(windowWidth / 2.0 + mapPanel.x + mapPanel.userX, windowHeight / 2.0 - mapPanel.y - mapPanel.userY);
	m.scale(mapPanel.scale, mapPanel.scale);
	m.rotate(-(mapPanel.angle + mapPanel.userAngle));

	painter->begin(paintDevice);

	if (renderMode != RenderMode::MAP)
	{
		painter->setClipping(true);
		painter->setClipRect(0, 0, (int)(mapPanel.relativeWidth * windowWidth + 0.5), (int)windowHeight);
	}
	else
		painter->setClipping(false);

	painter->setPen(pen);
	painter->setWorldMatrix(m);
	painter->drawPath(*routePath);
	painter->end();
}

void Renderer::renderPanel(Panel* panel)
{
	panel->program->bind();

	if (panel->vertexMatrixUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->vertexMatrixUniform, panel->vertexMatrix);

	if (panel->textureSamplerUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->textureSamplerUniform, 0);

	if (panel->textureWidthUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->textureWidthUniform, (float)panel->textureWidth);

	if (panel->textureHeightUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->textureHeightUniform, (float)panel->textureHeight);

	if (panel->texelWidthUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->texelWidthUniform, (float)panel->texelWidth);

	if (panel->texelHeightUniform >= 0)
		panel->program->setUniformValue((GLuint)panel->texelHeightUniform, (float)panel->texelHeight);

	panel->buffer->bind();
	panel->texture->bind();

	int* textureCoordinateOffset = (int*)(sizeof(GLfloat) * 12);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(panel->vertexPositionAttribute, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glVertexAttribPointer(panel->vertexTextureCoordinateAttribute, 2, GL_FLOAT, GL_FALSE, 0, textureCoordinateOffset);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	panel->texture->release();
	panel->buffer->release();
	panel->program->release();
}

Panel* Renderer::getVideoPanel()
{
	return &videoPanel;
}

Panel* Renderer::getMapPanel()
{
	return &mapPanel;
}

RenderMode Renderer::getRenderMode() const
{
	return renderMode;
}

void Renderer::setRenderMode(RenderMode mode)
{
	renderMode = mode;
}

void Renderer::setFlipOutput(bool value)
{
	paintDevice->setPaintFlipped(value);
	shouldFlipOutput = value;
}

void Renderer::setIsEncoding(bool value)
{
	isEncoding = value;
}

void Renderer::toggleShowInfoPanel()
{
	showInfoPanel = !showInfoPanel;
}

void Renderer::requestFullClear()
{
	fullClearRequested = true;
}
