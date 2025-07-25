#pragma once

#ifndef DOXYGEN_GENERATING_OUTPUT
#define NCINE_INCLUDE_OPENGL
#include "../../CommonHeaders.h"
#endif

#include "../../../Main.h"

#include <Containers/StringView.h>

#if defined(DEATH_TRACE) && defined(DEATH_TRACE_VERBOSE_GL)
#	define GL_LOG_ERRORS()										\
		do {													\
			GLenum __err = glGetError();						\
			if (__err != GL_NO_ERROR) {							\
				LOGW("OpenGL returned error: {}", __err);		\
			}													\
		} while (false)
#else
#	define GL_LOG_ERRORS() do {} while (false)
#endif

using namespace Death::Containers;

namespace nCine
{
	class IGfxCapabilities;

	/// Handles OpenGL debug functions
	class GLDebug
	{
	public:
		enum class LabelTypes
		{
#if defined(DEATH_TARGET_APPLE)
			Buffer,
			Shader,
			Program,
			VertexArray,
			Query,
			ProgramPipeline,
			TransformFeedback,
			Sampler,
			Texture,
			RenderBuffer,
			FrameBuffer
#else
			TransformFeedback = GL_TRANSFORM_FEEDBACK,
			Texture = GL_TEXTURE,
			RenderBuffer = GL_RENDERBUFFER,
			FrameBuffer = GL_FRAMEBUFFER,
#	if ((defined(DEATH_TARGET_ANDROID) && __ANDROID_API__ >= 21) || (!defined(DEATH_TARGET_ANDROID) && defined(WITH_OPENGLES))) && GL_ES_VERSION_3_0
			Buffer = GL_BUFFER_KHR,
			Shader = GL_SHADER_KHR,
			Program = GL_PROGRAM_KHR,
			VertexArray = GL_VERTEX_ARRAY_KHR,
			Query = GL_QUERY_KHR,
			ProgramPipeline = GL_PROGRAM_PIPELINE_KHR,
			Sampler = GL_SAMPLER_KHR
#	else
			Buffer = GL_BUFFER,
			Shader = GL_SHADER,
			Program = GL_PROGRAM,
			VertexArray = GL_VERTEX_ARRAY,
			Query = GL_QUERY,
			ProgramPipeline = GL_PROGRAM_PIPELINE,
			Sampler = GL_SAMPLER
#	endif
#endif
		};

		/// Scoped group for OpenGL debug messages
		class ScopedGroup
		{
		public:
			explicit ScopedGroup(StringView message) {
				PushGroup(message);
			}
			~ScopedGroup() {
				PopGroup();
			}
		};

		static void Init(const IGfxCapabilities& gfxCaps);
		static inline void Reset() {
			debugGroupId_ = 0;
		}

		static inline bool IsAvailable() {
			return debugAvailable_;
		}

		static void PushGroup(StringView message);
		static void PopGroup();
		static void MessageInsert(StringView message);

		static void SetObjectLabel(LabelTypes identifier, GLuint name, StringView label);
		static void GetObjectLabel(LabelTypes identifier, GLuint name, GLsizei bufSize, GLsizei* length, char* label);

		static inline std::int32_t GetMaxLabelLength() {
			return maxLabelLength_;
		}

	private:
		static bool debugAvailable_;
		static GLuint debugGroupId_;
		static std::int32_t maxLabelLength_;

		/// Enables OpenGL debug output and setup a callback function to log messages
		static void EnableDebugOutput();
	};

}
