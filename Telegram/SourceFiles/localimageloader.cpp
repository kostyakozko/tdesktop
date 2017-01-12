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
#include "stdafx.h"
#include "localimageloader.h"
#include "ui/filedialog.h"
#include "media/media_audio.h"

#include "boxes/send_files_box.h"
#include "media/media_clip_reader.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "lang.h"
#include "boxes/confirmbox.h"

TaskQueue::TaskQueue(QObject *parent, int32 stopTimeoutMs) : QObject(parent), _thread(0), _worker(0), _stopTimer(0) {
	if (stopTimeoutMs > 0) {
		_stopTimer = new QTimer(this);
		connect(_stopTimer, SIGNAL(timeout()), this, SLOT(stop()));
		_stopTimer->setSingleShot(true);
		_stopTimer->setInterval(stopTimeoutMs);
	}
}

TaskId TaskQueue::addTask(TaskPtr task) {
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		_tasksToProcess.push_back(task);
	}

	wakeThread();

	return task->id();
}

void TaskQueue::addTasks(const TasksList &tasks) {
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		_tasksToProcess.append(tasks);
	}

	wakeThread();
}

void TaskQueue::wakeThread() {
	if (!_thread) {
		_thread = new QThread();

		_worker = new TaskQueueWorker(this);
		_worker->moveToThread(_thread);

		connect(this, SIGNAL(taskAdded()), _worker, SLOT(onTaskAdded()));
		connect(_worker, SIGNAL(taskProcessed()), this, SLOT(onTaskProcessed()));

		_thread->start();
	}
	if (_stopTimer) _stopTimer->stop();
	emit taskAdded();
}

void TaskQueue::cancelTask(TaskId id) {
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		for (int32 i = 0, l = _tasksToProcess.size(); i != l; ++i) {
			if (_tasksToProcess.at(i)->id() == id) {
				_tasksToProcess.removeAt(i);
				return;
			}
		}
	}
	QMutexLocker lock(&_tasksToFinishMutex);
	for (int32 i = 0, l = _tasksToFinish.size(); i != l; ++i) {
		if (_tasksToFinish.at(i)->id() == id) {
			_tasksToFinish.removeAt(i);
			return;
		}
	}
}

void TaskQueue::onTaskProcessed() {
	do {
		TaskPtr task;
		{
			QMutexLocker lock(&_tasksToFinishMutex);
			if (_tasksToFinish.isEmpty()) break;
			task = _tasksToFinish.front();
			_tasksToFinish.pop_front();
		}
		task->finish();
	} while (true);

	if (_stopTimer) {
		QMutexLocker lock(&_tasksToProcessMutex);
		if (_tasksToProcess.isEmpty()) {
			_stopTimer->start();
		}
	}
}

void TaskQueue::stop() {
	if (_thread) {
		_thread->requestInterruption();
		_thread->quit();
		DEBUG_LOG(("Waiting for taskThread to finish"));
		_thread->wait();
		delete _worker;
		delete _thread;
		_worker = 0;
		_thread = 0;
	}
	_tasksToProcess.clear();
	_tasksToFinish.clear();
}

TaskQueue::~TaskQueue() {
	stop();
	delete _stopTimer;
}

void TaskQueueWorker::onTaskAdded() {
	if (_inTaskAdded) return;
	_inTaskAdded = true;

	bool someTasksLeft = false;
	do {
		TaskPtr task;
		{
			QMutexLocker lock(&_queue->_tasksToProcessMutex);
			if (!_queue->_tasksToProcess.isEmpty()) {
				task = _queue->_tasksToProcess.front();
			}
		}

		if (task) {
			task->process();
			bool emitTaskProcessed = false;
			{
				QMutexLocker lockToProcess(&_queue->_tasksToProcessMutex);
				if (!_queue->_tasksToProcess.isEmpty() && _queue->_tasksToProcess.front() == task) {
					_queue->_tasksToProcess.pop_front();
					someTasksLeft = !_queue->_tasksToProcess.isEmpty();

					QMutexLocker lockToFinish(&_queue->_tasksToFinishMutex);
					emitTaskProcessed = _queue->_tasksToFinish.isEmpty();
					_queue->_tasksToFinish.push_back(task);
				}
			}
			if (emitTaskProcessed) {
				emit taskProcessed();
			}
		}
		QCoreApplication::processEvents();
	} while (someTasksLeft && !thread()->isInterruptionRequested());

	_inTaskAdded = false;
}

FileLoadTask::FileLoadTask(const QString &filepath, SendMediaType type, const FileLoadTo &to, const QString &caption) : _id(rand_value<uint64>())
, _to(to)
, _filepath(filepath)
, _type(type)
, _caption(caption) {
}

FileLoadTask::FileLoadTask(const QByteArray &content, const QImage &image, SendMediaType type, const FileLoadTo &to, const QString &caption) : _id(rand_value<uint64>())
, _to(to)
, _content(content)
, _image(image)
, _type(type)
, _caption(caption) {
}

FileLoadTask::FileLoadTask(const QByteArray &voice, int32 duration, const VoiceWaveform &waveform, const FileLoadTo &to, const QString &caption) : _id(rand_value<uint64>())
, _to(to)
, _content(voice)
, _duration(duration)
, _waveform(waveform)
, _type(SendMediaType::Audio)
, _caption(caption) {
}

void FileLoadTask::process() {
	const QString stickerMime = qsl("image/webp");

	_result = MakeShared<FileLoadResult>(_id, _to, _caption);

	QString filename, filemime;
	qint64 filesize = 0;
	QByteArray filedata;

	uint64 thumbId = 0;
	QString thumbname = "thumb.jpg";
	QByteArray thumbdata;

	auto animated = false;
	auto song = false;
	auto gif = false;
	auto voice = (_type == SendMediaType::Audio);
	auto fullimage = base::take(_image);

	if (!_filepath.isEmpty()) {
		QFileInfo info(_filepath);
		if (info.isDir()) {
			_result->filesize = -1;
			return;
		}
		filesize = info.size();
		filemime = mimeTypeForFile(info).name();
		filename = info.fileName();
		auto opaque = (filemime != stickerMime);
		fullimage = App::readImage(_filepath, 0, opaque, &animated);
	} else if (!_content.isEmpty()) {
		filesize = _content.size();
		if (voice) {
			filename = filedialogDefaultName(qsl("audio"), qsl(".ogg"), QString(), true);
			filemime = "audio/ogg";
		} else {
			auto mimeType = mimeTypeForData(_content);
			filemime = mimeType.name();
			if (filemime != stickerMime) {
				fullimage = Images::prepareOpaque(std_::move(fullimage));
			}
			if (filemime == "image/jpeg") {
				filename = filedialogDefaultName(qsl("photo"), qsl(".jpg"), QString(), true);
			} else if (filemime == "image/png") {
				filename = filedialogDefaultName(qsl("image"), qsl(".png"), QString(), true);
			} else {
				QString ext;
				QStringList patterns = mimeType.globPatterns();
				if (!patterns.isEmpty()) {
					ext = patterns.front().replace('*', QString());
				}
				filename = filedialogDefaultName(qsl("file"), ext, QString(), true);
			}
		}
	} else if (!fullimage.isNull() && fullimage.width() > 0) {
		if (_type == SendMediaType::Photo) {
			auto w = fullimage.width(), h = fullimage.height();
			if (w >= 20 * h || h >= 20 * w) {
				_type = SendMediaType::File;
			} else {
				filesize = -1; // Fill later.
				filemime = mimeTypeForName("image/jpeg").name();
				filename = filedialogDefaultName(qsl("image"), qsl(".jpg"), QString(), true);
			}
		}
		if (_type == SendMediaType::File) {
			filemime = mimeTypeForName("image/png").name();
			filename = filedialogDefaultName(qsl("image"), qsl(".png"), QString(), true);
			{
				QBuffer buffer(&_content);
				fullimage.save(&buffer, "PNG");
			}
			filesize = _content.size();
		}
		fullimage = Images::prepareOpaque(std_::move(fullimage));
	}
	_result->filesize = (int32)qMin(filesize, qint64(INT_MAX));

	if (!filesize || filesize > App::kFileSizeLimit) {
		return;
	}

	PreparedPhotoThumbs photoThumbs;
	QVector<MTPPhotoSize> photoSizes;
	QPixmap thumb;

	QVector<MTPDocumentAttribute> attributes(1, MTP_documentAttributeFilename(MTP_string(filename)));

	MTPPhotoSize thumbSize(MTP_photoSizeEmpty(MTP_string("")));
	MTPPhoto photo(MTP_photoEmpty(MTP_long(0)));
	MTPDocument document(MTP_documentEmpty(MTP_long(0)));

	if (!voice) {
		if (filemime == qstr("audio/mp3") || filemime == qstr("audio/m4a") || filemime == qstr("audio/aac") || filemime == qstr("audio/ogg") || filemime == qstr("audio/flac") ||
			filename.endsWith(qstr(".mp3"), Qt::CaseInsensitive) || filename.endsWith(qstr(".m4a"), Qt::CaseInsensitive) ||
			filename.endsWith(qstr(".aac"), Qt::CaseInsensitive) || filename.endsWith(qstr(".ogg"), Qt::CaseInsensitive) ||
			filename.endsWith(qstr(".flac"), Qt::CaseInsensitive)) {
			QImage cover;
			QByteArray coverBytes, coverFormat;
			MTPDocumentAttribute audioAttribute = audioReadSongAttributes(_filepath, _content, cover, coverBytes, coverFormat);
			if (audioAttribute.type() == mtpc_documentAttributeAudio) {
				attributes.push_back(audioAttribute);
				song = true;
				if (!cover.isNull()) { // cover to thumb
					int32 cw = cover.width(), ch = cover.height();
					if (cw < 20 * ch && ch < 20 * cw) {
						QPixmap full = (cw > 90 || ch > 90) ? App::pixmapFromImageInPlace(cover.scaled(90, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : App::pixmapFromImageInPlace(std_::move(cover));
						{
							QByteArray thumbFormat = "JPG";
							int32 thumbQuality = 87;

							QBuffer buffer(&thumbdata);
							full.save(&buffer, thumbFormat, thumbQuality);
						}

						thumb = full;
						thumbSize = MTP_photoSize(MTP_string(""), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0));

						thumbId = rand_value<uint64>();
					}
				}
			}
		}
		if (filemime == qstr("video/mp4") || filename.endsWith(qstr(".mp4"), Qt::CaseInsensitive) || animated) {
			QImage cover;
			MTPDocumentAttribute animatedAttribute = Media::Clip::readAttributes(_filepath, _content, cover);
			if (animatedAttribute.type() == mtpc_documentAttributeVideo) {
				int32 cw = cover.width(), ch = cover.height();
				if (cw < 20 * ch && ch < 20 * cw) {
					attributes.push_back(MTP_documentAttributeAnimated());
					attributes.push_back(animatedAttribute);
					gif = true;

					QPixmap full = (cw > 90 || ch > 90) ? App::pixmapFromImageInPlace(cover.scaled(90, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : App::pixmapFromImageInPlace(std_::move(cover));
					{
						QByteArray thumbFormat = "JPG";
						int32 thumbQuality = 87;

						QBuffer buffer(&thumbdata);
						full.save(&buffer, thumbFormat, thumbQuality);
					}

					thumb = full;
					thumbSize = MTP_photoSize(MTP_string(""), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0));

					thumbId = rand_value<uint64>();

					if (filename.endsWith(qstr(".mp4"), Qt::CaseInsensitive)) {
						filemime = qstr("video/mp4");
					}
				}
			}
		}
	}

	if (!fullimage.isNull() && fullimage.width() > 0 && !song && !gif && !voice) {
		auto w = fullimage.width(), h = fullimage.height();
		attributes.push_back(MTP_documentAttributeImageSize(MTP_int(w), MTP_int(h)));

		if (w < 20 * h && h < 20 * w) {
			if (animated) {
				attributes.push_back(MTP_documentAttributeAnimated());
			} else if (_type != SendMediaType::File) {
				auto thumb = (w > 100 || h > 100) ? App::pixmapFromImageInPlace(fullimage.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : QPixmap::fromImage(fullimage);
				photoThumbs.insert('s', thumb);
				photoSizes.push_back(MTP_photoSize(MTP_string("s"), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(thumb.width()), MTP_int(thumb.height()), MTP_int(0)));

				auto medium = (w > 320 || h > 320) ? App::pixmapFromImageInPlace(fullimage.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : QPixmap::fromImage(fullimage);
				photoThumbs.insert('m', medium);
				photoSizes.push_back(MTP_photoSize(MTP_string("m"), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(medium.width()), MTP_int(medium.height()), MTP_int(0)));

				auto full = (w > 1280 || h > 1280) ? App::pixmapFromImageInPlace(fullimage.scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : QPixmap::fromImage(fullimage);
				photoThumbs.insert('y', full);
				photoSizes.push_back(MTP_photoSize(MTP_string("y"), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0)));

				{
					QBuffer buffer(&filedata);
					full.save(&buffer, "JPG", 87);
				}

				MTPDphoto::Flags photoFlags = 0;
				photo = MTP_photo(MTP_flags(photoFlags), MTP_long(_id), MTP_long(0), MTP_int(unixtime()), MTP_vector<MTPPhotoSize>(photoSizes));

				if (filesize < 0) {
					filesize = _result->filesize = filedata.size();
				}
			}

			QByteArray thumbFormat = "JPG";
			int32 thumbQuality = 87;
			if (!animated && filemime == stickerMime && w > 0 && h > 0 && w <= StickerMaxSize && h <= StickerMaxSize && filesize < StickerInMemory) {
				MTPDdocumentAttributeSticker::Flags stickerFlags = 0;
				attributes.push_back(MTP_documentAttributeSticker(MTP_flags(stickerFlags), MTP_string(""), MTP_inputStickerSetEmpty(), MTPMaskCoords()));
				thumbFormat = "webp";
				thumbname = qsl("thumb.webp");
			}

			QPixmap full = (w > 90 || h > 90) ? App::pixmapFromImageInPlace(fullimage.scaled(90, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)) : QPixmap::fromImage(fullimage, Qt::ColorOnly);

			{
				QBuffer buffer(&thumbdata);
				full.save(&buffer, thumbFormat, thumbQuality);
			}

			thumb = full;
			thumbSize = MTP_photoSize(MTP_string(""), MTP_fileLocationUnavailable(MTP_long(0), MTP_int(0), MTP_long(0)), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0));

			thumbId = rand_value<uint64>();
		}
	}

	if (voice) {
		attributes[0] = MTP_documentAttributeAudio(MTP_flags(MTPDdocumentAttributeAudio::Flag::f_voice | MTPDdocumentAttributeAudio::Flag::f_waveform), MTP_int(_duration), MTPstring(), MTPstring(), MTP_bytes(documentWaveformEncode5bit(_waveform)));
		attributes.resize(1);
		document = MTP_document(MTP_long(_id), MTP_long(0), MTP_int(unixtime()), MTP_string(filemime), MTP_int(filesize), thumbSize, MTP_int(MTP::maindc()), MTP_int(0), MTP_vector<MTPDocumentAttribute>(attributes));
	} else if (_type != SendMediaType::Photo) {
		document = MTP_document(MTP_long(_id), MTP_long(0), MTP_int(unixtime()), MTP_string(filemime), MTP_int(filesize), thumbSize, MTP_int(MTP::maindc()), MTP_int(0), MTP_vector<MTPDocumentAttribute>(attributes));
		_type = SendMediaType::File;
	}

	_result->type = _type;
	_result->filepath = _filepath;
	_result->content = _content;

	_result->filename = filename;
	_result->filemime = filemime;
	_result->setFileData(filedata);

	_result->thumbId = thumbId;
	_result->thumbname = thumbname;
	_result->setThumbData(thumbdata);
	_result->thumb = thumb;

	_result->photo = photo;
	_result->document = document;
	_result->photoThumbs = photoThumbs;
}

void FileLoadTask::finish() {
	if (!_result || !_result->filesize) {
		Ui::show(Box<InformBox>(lng_send_image_empty(lt_name, _filepath)), KeepOtherLayers);
	} else if (_result->filesize == -1) { // dir
		Ui::show(Box<InformBox>(lng_send_folder(lt_name, QFileInfo(_filepath).dir().dirName())), KeepOtherLayers);
	} else if (_result->filesize > App::kFileSizeLimit) {
		Ui::show(Box<InformBox>(lng_send_image_too_large(lt_name, _filepath)), KeepOtherLayers);
	} else if (App::main()) {
		App::main()->onSendFileConfirm(_result);
	}
}
