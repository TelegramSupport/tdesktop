/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "storage/storage_media_prepare.h"

#include "platform/platform_file_utilities.h"
#include "base/task_queue.h"
#include "storage/localimageloader.h"

namespace Storage {
namespace {

constexpr auto kMaxAlbumCount = 10;

bool HasExtensionFrom(const QString &file, const QStringList &extensions) {
	for (const auto &extension : extensions) {
		const auto ext = file.right(extension.size());
		if (ext.compare(extension, Qt::CaseInsensitive) == 0) {
			return true;
		}
	}
	return false;
}

bool ValidPhotoForAlbum(const FileMediaInformation::Image &image) {
	if (image.animated) {
		return false;
	}
	const auto width = image.data.width();
	const auto height = image.data.height();
	return ValidateThumbDimensions(width, height);
}

bool ValidVideoForAlbum(const FileMediaInformation::Video &video) {
	const auto width = video.thumbnail.width();
	const auto height = video.thumbnail.height();
	return ValidateThumbDimensions(width, height);
}

bool PrepareAlbumMediaIsWaiting(
		QSemaphore &semaphore,
		PreparedFile &file,
		int previewWidth) {
	// Use some special thread queue, like a separate QThreadPool.
	base::TaskQueue::Normal().Put([&, previewWidth] {
		const auto guard = gsl::finally([&] { semaphore.release(); });
		if (!file.path.isEmpty()) {
			file.mime = mimeTypeForFile(QFileInfo(file.path)).name();
			file.information = FileLoadTask::ReadMediaInformation(
				file.path,
				QByteArray(),
				file.mime);
		} else if (!file.content.isEmpty()) {
			file.mime = mimeTypeForData(file.content).name();
			file.information = FileLoadTask::ReadMediaInformation(
				QString(),
				file.content,
				file.mime);
		} else {
			Assert(file.information != nullptr);
		}

		using Image = FileMediaInformation::Image;
		using Video = FileMediaInformation::Video;
		if (const auto image = base::get_if<Image>(
				&file.information->media)) {
			if (ValidPhotoForAlbum(*image)) {
				file.preview = image->data.scaledToWidth(
					std::min(previewWidth, convertScale(image->data.width()))
						* cIntRetinaFactor(),
					Qt::SmoothTransformation);
				file.preview.setDevicePixelRatio(cRetinaFactor());
				file.type = PreparedFile::AlbumType::Photo;
			}
		} else if (const auto video = base::get_if<Video>(
				&file.information->media)) {
			if (ValidVideoForAlbum(*video)) {
				auto blurred = Images::prepareBlur(video->thumbnail);
				file.preview = std::move(blurred).scaledToWidth(
					previewWidth * cIntRetinaFactor(),
					Qt::SmoothTransformation);
				file.preview.setDevicePixelRatio(cRetinaFactor());
				file.type = PreparedFile::AlbumType::Video;
			}
		}
	});
	return true;
}

void PrepareAlbum(PreparedList &result, int previewWidth) {
	const auto count = int(result.files.size());
	if (count > kMaxAlbumCount) {
		return;
	}

	result.albumIsPossible = (count > 1);
	auto waiting = 0;
	QSemaphore semaphore;
	for (auto &file : result.files) {
		if (PrepareAlbumMediaIsWaiting(semaphore, file, previewWidth)) {
			++waiting;
		}
	}
	if (waiting > 0) {
		semaphore.acquire(waiting);
		if (result.albumIsPossible) {
			const auto badIt = ranges::find(
				result.files,
				PreparedFile::AlbumType::None,
				[](const PreparedFile &file) { return file.type; });
			result.albumIsPossible = (badIt == result.files.end());
		}
	}
}

} // namespace

bool ValidateThumbDimensions(int width, int height) {
	return (width > 0)
		&& (height > 0)
		&& (width < 20 * height)
		&& (height < 20 * width);
}

PreparedFile::PreparedFile(const QString &path) : path(path) {
}

PreparedFile::PreparedFile(PreparedFile &&other) = default;

PreparedFile &PreparedFile::operator=(PreparedFile &&other) = default;

PreparedFile::~PreparedFile() = default;

MimeDataState ComputeMimeDataState(const QMimeData *data) {
	if (!data
		|| data->hasFormat(qsl("application/x-td-forward-selected"))
		|| data->hasFormat(qsl("application/x-td-forward-pressed"))
		|| data->hasFormat(qsl("application/x-td-forward-pressed-link"))) {
		return MimeDataState::None;
	}

	if (data->hasImage()) {
		return MimeDataState::Image;
	}

	const auto uriListFormat = qsl("text/uri-list");
	if (!data->hasFormat(uriListFormat)) {
		return MimeDataState::None;
	}

	const auto &urls = data->urls();
	if (urls.isEmpty()) {
		return MimeDataState::None;
	}

	const auto imageExtensions = cImgExtensions();
	auto files = QStringList();
	auto allAreSmallImages = true;
	for (const auto &url : urls) {
		if (!url.isLocalFile()) {
			return MimeDataState::None;
		}
		const auto file = Platform::File::UrlToLocal(url);

		const auto info = QFileInfo(file);
		if (info.isDir()) {
			return MimeDataState::None;
		}

		const auto filesize = info.size();
		if (filesize > App::kFileSizeLimit) {
			return MimeDataState::None;
		} else if (allAreSmallImages) {
			if (filesize > App::kImageSizeLimit) {
				allAreSmallImages = false;
			} else if (!HasExtensionFrom(file, imageExtensions)) {
				allAreSmallImages = false;
			}
		}
	}
	return allAreSmallImages
		? MimeDataState::PhotoFiles
		: MimeDataState::Files;
}

PreparedList PrepareMediaList(const QList<QUrl> &files, int previewWidth) {
	auto locals = QStringList();
	locals.reserve(files.size());
	for (const auto &url : files) {
		if (!url.isLocalFile()) {
			return {
				PreparedList::Error::NonLocalUrl,
				url.toDisplayString()
			};
		}
		locals.push_back(Platform::File::UrlToLocal(url));
	}
	return PrepareMediaList(locals, previewWidth);
}

PreparedList PrepareMediaList(const QStringList &files, int previewWidth) {
	auto result = PreparedList();
	result.files.reserve(files.size());
	const auto extensionsToCompress = cExtensionsForCompress();
	for (const auto &file : files) {
		const auto fileinfo = QFileInfo(file);
		const auto filesize = fileinfo.size();
		if (fileinfo.isDir()) {
			return {
				PreparedList::Error::Directory,
				file
			};
		} else if (filesize <= 0) {
			return {
				PreparedList::Error::EmptyFile,
				file
			};
		} else if (filesize > App::kFileSizeLimit) {
			return {
				PreparedList::Error::TooLargeFile,
				file
			};
		}
		const auto toCompress = HasExtensionFrom(file, extensionsToCompress);
		if (filesize > App::kImageSizeLimit || !toCompress) {
			result.allFilesForCompress = false;
		}
		result.files.emplace_back(file);
	}
	PrepareAlbum(result, previewWidth);
	return result;
}

PreparedList PrepareMediaFromImage(
		QImage &&image,
		QByteArray &&content,
		int previewWidth) {
	auto result = Storage::PreparedList();
	result.allFilesForCompress = ValidateThumbDimensions(
		image.width(),
		image.height());
	auto file = PreparedFile(QString());
	file.content = content;
	if (file.content.isEmpty()) {
		file.information = std::make_unique<FileMediaInformation>();
		const auto animated = false;
		FileLoadTask::FillImageInformation(
			std::move(image),
			animated,
			file.information);
	}
	result.files.push_back(std::move(file));
	PrepareAlbum(result, previewWidth);
	return result;
}

PreparedList PreparedList::Reordered(
		PreparedList &&list,
		std::vector<int> order) {
	Expects(list.error == PreparedList::Error::None);
	Expects(list.files.size() == order.size());

	auto result = PreparedList(list.error, list.errorData);
	result.albumIsPossible = list.albumIsPossible;
	result.allFilesForCompress = list.allFilesForCompress;
	result.files.reserve(list.files.size());
	for (auto index : order) {
		result.files.push_back(std::move(list.files[index]));
	}
	return result;
}

} // namespace Storage

