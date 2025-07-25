#include "TextureLoaderKtx.h"

using namespace Death::IO;

namespace nCine
{
	std::uint8_t TextureLoaderKtx::fileIdentifier_[] = {
		0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
	}; // "«KTX 11»\r\n\x1A\n"};

	TextureLoaderKtx::TextureLoaderKtx(std::unique_ptr<Stream> fileHandle)
		: ITextureLoader(std::move(fileHandle))
	{
		KtxHeader header;

		if (!fileHandle_->IsValid()) {
			return;
		}

		const bool headerRead = readHeader(header);
		DEATH_ASSERT(headerRead, "KTX header cannot be read", );
		const bool formatParsed = parseFormat(header);
		DEATH_ASSERT(formatParsed, "KTX format cannot be parsed", );

		hasLoaded_ = true;
	}

	bool TextureLoaderKtx::readHeader(KtxHeader& header)
	{
		bool checkPassed = true;

		// KTX header is 64 bytes long
		fileHandle_->Read(&header, 64);

		for (int i = 0; i < KtxIdentifierLength; i++) {
			if (header.identifier[i] != fileIdentifier_[i]) {
				checkPassed = false;
			}
		}

		DEATH_ASSERT(checkPassed, "Invalid KTX signature", false);
		// Checking for the header identifier
		DEATH_ASSERT(header.endianess != 0x01020304, "File endianess doesn't match machine one", false);

		// Accounting for key-value data and `UInt32 imageSize` from first MIP level
		headerSize_ = 64 + Stream::Uint32FromLE(header.bytesOfKeyValueData) + 4;
		width_ = Stream::Uint32FromLE(header.pixelWidth);
		height_ = Stream::Uint32FromLE(header.pixelHeight);
		mipMapCount_ = Stream::Uint32FromLE(header.numberOfMipmapLevels);

		return true;
	}

	bool TextureLoaderKtx::parseFormat(const KtxHeader& header)
	{
		const GLenum internalFormat = Stream::Uint32FromLE(header.glInternalFormat);
		const GLenum type = Stream::Uint32FromLE(header.glType);

		loadPixels(internalFormat, type);

		if (mipMapCount_ > 1) {
			LOGI("MIP Maps: {}", mipMapCount_);
			mipDataOffsets_ = std::make_unique<std::uint32_t[]>(mipMapCount_);
			mipDataSizes_ = std::make_unique<std::uint32_t[]>(mipMapCount_);
			std::uint32_t dataSizesSum = TextureFormat::calculateMipSizes(internalFormat, width_, height_, mipMapCount_, mipDataOffsets_.get(), mipDataSizes_.get());

			// HACK: accounting for `UInt32 imageSize` on top of each MIP level
			// Excluding the first one, already taken into account in header size
			for (std::int32_t i = 1; i < mipMapCount_; i++) {
				mipDataOffsets_[i] += 4 * i;
			}
			dataSizesSum += 4 * mipMapCount_;

			if (dataSizesSum != dataSize_) {
				LOGW("The sum of MIP maps size ({}) is different than texture total data ({})", dataSizesSum, dataSize_);
			}
		}

		return true;
	}
}
