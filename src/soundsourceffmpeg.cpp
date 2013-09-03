/* -*- mode:C++; indent-tabs-mode:t; tab-width:8; c-basic-offset:4; -*- */
/***************************************************************************
                          soundsourceffmpeg.cpp -  ffmpeg decoder
                             -------------------
    copyright            : (C) 2007 by Cedric GESTES
                           (C) 2012-2013 by Tuukka Pasanen
    email                : ctaf42@gmail.com

    This one tested with FFMPEG 0.10/0.11/1.0/1.1/GIT
    FFMPEG below 0.10 WON'T work. If you like to it work you can
    allways send a patch but it's mostly not worth it!
***************************************************************************/

/***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************/

#include "trackinfoobject.h"
#include "soundsourceffmpeg.h"

//#ifdef __WINDOWS__
//#include <io.h>
//#include <fcntl.h>
//#endif

#include <QDebug>

static QMutex ffmpegmutex;

// If some point of time we don't want
// to use buffering (mostly for debug)
// We turn it off
//5#define FFMPEG_USE_BUFFER_PLAY

#define FFMPEG_MP3_FRAME_SIZE 1152
#define FFMPEG_MP4_FRAME_SIZE 1024
#define FFMPEG_OPUS_FRAME_SIZE 960
#define FFMPEG_OGG_FRAME_SIZE 128

SoundSourceFFmpeg::SoundSourceFFmpeg(QString filename)
    : Mixxx::SoundSource(filename)
    , m_qFilename(filename) {
    m_iAudioStream = -1;
    filelength = -1;
    m_pFormatCtx = NULL;
    m_pIformat = NULL;
    m_pCodecCtx = NULL;
    m_pCodec = NULL;
    m_iOffset = 0;
    m_bIsSeeked = FALSE;
    m_iReadedBytes = 0;
    m_iNextMixxxPCMPoint = -1;
    m_pResample = NULL;
    m_fMixxBytePosition = 0;

    this->setType(filename.section(".",-1).toLower());
}

SoundSourceFFmpeg::~SoundSourceFFmpeg() {
    if (m_pCodecCtx != NULL) {
        avcodec_close(m_pCodecCtx);
        avformat_close_input(&m_pFormatCtx);
    }

    if (m_pResample != NULL) {
        delete m_pResample;
    }

};

AVCodecContext *SoundSourceFFmpeg::getCodecContext() {
    return m_pCodecCtx;
}

AVFormatContext *SoundSourceFFmpeg::getFormatContext() {
    return m_pFormatCtx;
}

int SoundSourceFFmpeg::getAudioStreamIndex() {
    return m_iAudioStream;
}


void SoundSourceFFmpeg::lock() {
    ffmpegmutex.lock();
}

void SoundSourceFFmpeg::unlock() {
    ffmpegmutex.unlock();
}

double SoundSourceFFmpeg::convertFfmpegToMixxx(double pos,
        const AVRational &time_base) {
    return (pos / (double)time_base.den * (double)this->getSampleRate() *
            (double)2.);
}

double SoundSourceFFmpeg::convertMixxxToFfmpeg(double pos,
        const AVRational &time_base) {
    return (pos / (double)this->getSampleRate() / (double)2.) *
           (double)time_base.den;
}

int64_t SoundSourceFFmpeg::ffmpeg2mixxx(int64_t pos,
                                        const AVRational &time_base) {
    int64_t l_lReturnValue = 0;

    l_lReturnValue = round(convertFfmpegToMixxx(pos, time_base));

    if ((l_lReturnValue % 4) != 0) {
        l_lReturnValue += 4 - (l_lReturnValue % 4);
    }

    return l_lReturnValue;
}

int64_t SoundSourceFFmpeg::mixxx2ffmpeg(int64_t pos,
                                        const AVRational &time_base) {
    int64_t l_lReturnValue = 0;
    l_lReturnValue = round(convertMixxxToFfmpeg(pos, time_base));
    return l_lReturnValue + (l_lReturnValue % 2);
}

int SoundSourceFFmpeg::open() {
    unsigned int i;

    m_iOffset = 0;
#ifdef __WINDOWS__
    // From Tobias: A Utf-8 string did not work on my Windows XP (German edition)
    // If you try this conversion, f.isValid() will return false in many cases
    // and processTaglibFile() will fail
    //
    // The method toLocal8Bit() returns the local 8-bit representation
    // of the string as a QByteArray. The returned byte array is undefined if
    // the string contains characters not supported
    // by the local 8-bit encoding.
    //
    // See https://ffmpeg.org/trac/ffmpeg/ticket/819 for relevant bug report.
    //
    QByteArray qBAFilename = m_qFilename.toLocal8Bit();
#else
    QByteArray qBAFilename = m_qFilename.toUtf8();
#endif
    // Initialize FFMPEG
    // FFmpegInit();

    qDebug() << "New SoundSourceFFmpeg :" << qBAFilename;

    m_pFormatCtx = avformat_alloc_context();

    // Open file and make m_pFormatCtx
    if (avformat_open_input(&m_pFormatCtx, qBAFilename.constData(), NULL,
                            NULL)!=0) {
        qDebug() << "av_open_input_file: cannot open" << qBAFilename;
        return ERR;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(m_pFormatCtx, NULL)<0) {
        qDebug() << "av_find_stream_info: cannot open" << qBAFilename;
        return ERR;
    }

    //debug only (Enable if needed)
    //av_dump_format(m_pFormatCtx, 0, qBAFilename.constData(), false);

    // Find the first video stream
    m_iAudioStream=-1;

    for (i=0; i<m_pFormatCtx->nb_streams; i++)
        if (m_pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            m_iAudioStream=i;
            break;
        }
    if (m_iAudioStream==-1) {
        qDebug() << "ffmpeg: cannot find an audio stream: cannot open" 
        << qBAFilename;
        return ERR;
    }

    // Get a pointer to the codec context for the video stream
    m_pCodecCtx=m_pFormatCtx->streams[m_iAudioStream]->codec;

    // Find the decoder for the audio stream
    if (!(m_pCodec=avcodec_find_decoder(m_pCodecCtx->codec_id))) {
        qDebug() << "ffmpeg: cannot find a decoder for" << qBAFilename;
        return ERR;
    }

    // qDebug() << "ffmpeg: opening the audio codec";
    //avcodec_open is not thread safe
    lock();
    if (avcodec_open2(m_pCodecCtx, m_pCodec, NULL)<0) {
        qDebug() << "ffmpeg:  cannot open" << qBAFilename;
        return ERR;
    }
    unlock();

    m_pResample = new EncoderFfmpegResample(m_pCodecCtx);
    m_pResample->open(m_pCodecCtx->sample_fmt, AV_SAMPLE_FMT_S16);

    this->setChannels(m_pCodecCtx->channels);
    this->setSampleRate(m_pCodecCtx->sample_rate);

    qDebug() << "ffmpeg: Samplerate: " << this->getSampleRate() << ", Channels: " <<
             this->getChannels() << "\n";
    if (this->getChannels() > 2) {
        qDebug() << "ffmpeg: No support for more than 2 channels!";
        return ERR;
    }
    filelength = (long int) ((double)m_pFormatCtx->duration * 2 / AV_TIME_BASE *
                             this->getSampleRate());

    if (this->getType().compare("mp3") == 0) {
        m_pCodecCtx->frame_size = FFMPEG_MP3_FRAME_SIZE;
    } else if (this->getType().compare("mp4") == 0  ||
               this->getType().compare("m4a")) {
        m_pCodecCtx->frame_size = FFMPEG_MP4_FRAME_SIZE;
    } else if (this->getType().compare("ogg") == 0) {
        m_pCodecCtx->frame_size = FFMPEG_OGG_FRAME_SIZE;
    } else if (this->getType().compare("opus") == 0) {
        m_pCodecCtx->frame_size = FFMPEG_OPUS_FRAME_SIZE;
    } else {
        m_pCodecCtx->frame_size = FFMPEG_MP3_FRAME_SIZE;
    }

    return OK;
}

long SoundSourceFFmpeg::seek(long filepos) {
    int ret = 0;
    int64_t fspos = 0;
    int64_t minus = filepos;
    AVRational time_base = m_pFormatCtx->streams[m_iAudioStream]->time_base;

#ifdef FFMPEG_USE_BUFFER_PLAY
    // We are playing just plainly ahead
    // No need for jumping around?
    // This is what is most of the times wanted
    // Jumping around is not.. so we have stuff
    // In cache play there and just go go go..
    if (m_iNextMixxxPCMPoint == filepos && filepos != 0) {
        m_iCurrentMixxTs = filepos;
        return filepos;
    }

    qDebug() << "Seeked to" << filepos << "Assumed:" << m_iNextMixxxPCMPoint;
    m_iNextMixxxPCMPoint = filepos;
#endif

    // qDebug() << "Seeked to" << filepos << "Assumed:" << m_iNextMixxxPCMPoint;
    // We seek we don't need buffering because we don't have
    // Anything that we need there..
    m_strBuffer.clear();


    if (filepos >=  (m_pCodecCtx->frame_size * 4)) {
        // Calculate position on 4608 based.. MP3 will be happy..
        minus = ((double)filepos) / (m_pCodecCtx->frame_size * 2);

        // They should allways be dividable by 2
        minus += (minus % 2);
        minus -= 2;

        minus *= (m_pCodecCtx->frame_size * 2);

        // PREGAP OF OPUS..
        //minus += 118;
    } else {
        minus = filepos;
    }

    fspos = mixxx2ffmpeg(minus, time_base);
    fspos = (int64_t) round(convertMixxxToFfmpeg(minus, time_base));
    m_iCurrentMixxTs = filepos;

    m_iOffset = 0;

    ret = avformat_seek_file    (m_pFormatCtx,
                                 m_iAudioStream,
                                 0,
                                 fspos,
                                 fspos,
                                 AVSEEK_FLAG_BACKWARD);


    avcodec_flush_buffers(m_pCodecCtx);

    if (ret < 0) {
        qDebug() << "ffmpeg: Seek ERROR ret(" << ret << ") filepos(" << filepos <<
                 "d).";
        return 0;
    }

    m_iOffset = ffmpeg2mixxx(fspos - m_pFormatCtx->streams[m_iAudioStream]->cur_dts,
                             time_base);

    m_bIsSeeked = TRUE;

    return filepos;
}

unsigned int SoundSourceFFmpeg::read(unsigned long size,
                                     const SAMPLE * destination) {
    // Is this really needed?
    //Q_ASSERT(size%2==0);

    char *pRead  = (char*) destination;

    // SAMPLE *dest   = (SAMPLE*) destination;
    QByteArray readByteArray;
    QBuffer readBuffer(&readByteArray);

    AVPacket l_SPacket;
    AVFrame *l_pFrame = avcodec_alloc_frame();

    // BAD.. we want to have stereo.. 2 channels allways!
    // If we don't have it.. we convert!!
    int64_t needed = (size * 2);
    unsigned int copysize = needed;
    unsigned int ret = 0;
    int readBytes = 0;
    int frameFinished=0;
    double toMixxPosSec = 0;
    double fromMixxPosSec = 0;
    double currentFFMPEGPosSec = 0;
    int64_t currentFFMPEGPosByte = 0;
    int64_t currentFFMPEGPosByte2 = 0;
    double l_fCurrentFFMPEGPosByte = 0;
    double currentBufferPosSec = 0;
    bool isFirstPacket = TRUE;
    bool m_bReadLoop = FALSE;

    // loop until requested number of samples has been retrieved
    //l_SPacket.data = NULL;
    //av_init_packet(&l_SPacket);

    // Just make sure everything is zeroed before use
    // Needless but..
    memset(pRead, 0x00, copysize);

    readByteArray.clear();

    readBuffer.open(QIODevice::ReadWrite);

    // If we don't seek like we don't on analyzer.. keep
    // place in mind..
    if (m_bIsSeeked == FALSE) {
        m_iCurrentMixxTs += size;
    }

    // Mostly for debug
    fromMixxPosSec = (((double)m_iCurrentMixxTs / (double)this->getSampleRate())) /
                     2;
    toMixxPosSec = ((((double)m_iCurrentMixxTs + (double)size) /
                     (double)this->getSampleRate())) / 2;

    // This is the next assumed point if don't seek anywhere
    m_iNextMixxxPCMPoint += size;

    if (m_strBuffer.size() > 0) {
        readBuffer.write(m_strBuffer.data(), m_strBuffer.size());
        m_strBuffer.clear();
        // So we buffered this amount of bytes from last time
        currentBufferPosSec = (((double)(readByteArray.size() / 2) /
                                (double)this->getSampleRate())) / 2;
        // We are in position at least this..
        currentFFMPEGPosSec = currentBufferPosSec + fromMixxPosSec;
        // qDebug() << "ffmpeg: BUFFERED Mixxx t: " << currentBufferPosSec << " currentFFMPEGPosSec: " << currentFFMPEGPosSec << "Bytes BUffered" << readByteArray.size();
    }

    //qDebug() << "ffmpeg: FROM Mixxx t: " << fromMixxPosSec << " (B: " 
    //<< m_iCurrentMixxTs << ")";
    //qDebug() << "ffmpeg: TO   Mixxx t: " << toMixxPosSec << " (B: "
    //<< (m_iCurrentMixxTs + size) << ")";

    // If packet is done then INIT..
    // Make sure we don't have any grab after seek to make us suffer!
    if ( m_bIsSeeked == TRUE || (l_SPacket.data == NULL && l_SPacket.size == 0)) {
        l_SPacket.data = NULL;
        l_SPacket.size = 0;
        av_init_packet(&l_SPacket);
    }

    //while (readByteArray.size() < needed)
    while (!m_bReadLoop) {
        readBytes = 0;

        if (av_read_frame(m_pFormatCtx, &l_SPacket) >= 0) {
            if (l_SPacket.stream_index==m_iAudioStream) {
                ret = avcodec_decode_audio4(m_pCodecCtx,l_pFrame,&frameFinished,&l_SPacket);

                if (ret <= 0) {
                    // An error or EOF occured,index break out and return what 
                    // we have so far.
                    qDebug() << "EOF!";
                    break;
                }

                //frame->
                if (frameFinished) {
                    m_pResample->reSample(l_pFrame);
                    readBytes = av_samples_get_buffer_size(NULL, m_pCodecCtx->channels,
                                                    l_pFrame->nb_samples,
                                                    m_pCodecCtx->sample_fmt, 1);

                    m_iReadedBytes += (readBytes / 2);

                    currentFFMPEGPosSec = l_SPacket.pts * av_q2d(
                              m_pFormatCtx->streams[m_iAudioStream]->time_base);
                    l_fCurrentFFMPEGPosByte = convertFfmpegToMixxx(l_SPacket.pts,
                              m_pFormatCtx->streams[m_iAudioStream]->time_base);
                    currentFFMPEGPosByte = round(l_fCurrentFFMPEGPosByte);
                    currentFFMPEGPosByte2 = (currentFFMPEGPosSec * (44100 * 2));

                    currentFFMPEGPosByte += (currentFFMPEGPosByte % 4);

                    // Now we really really know where we are.. so
                    // Calculate diffrence in stream (Ogg Vorbis..
                    // CBR works without)
                    if (isFirstPacket == TRUE && m_bIsSeeked == TRUE && 
                        l_SPacket.pts >= 0) {


                        if (m_iCurrentMixxTs > currentFFMPEGPosByte) {
                            m_iOffset = m_iCurrentMixxTs - (currentFFMPEGPosByte
                                                         - (m_pCodecCtx->frame_size
                                                            * 2));
                        }

                        m_iOffset *= 2;

                        needed = (size * this->getChannels()) + m_iOffset;

                        if (((int64_t) round(convertMixxxToFfmpeg(
                        l_fCurrentFFMPEGPosByte,
                        m_pFormatCtx->streams[m_iAudioStream]->time_base))) -
                        l_SPacket.pts != 0) {
                            int64_t l_iWarning1 = (int64_t) 
                            round(convertMixxxToFfmpeg(
                            l_fCurrentFFMPEGPosByte, 
                            m_pFormatCtx->streams[m_iAudioStream]->time_base));
                            
                            qDebug() << "ffmpeg: Warning: Diff sec: " << 
                                fromMixxPosSec -currentFFMPEGPosSec << 
                                " diff B: " << 
                                (m_iCurrentMixxTs - currentFFMPEGPosByte)
                                * 2 << " ! " << 
                                (int64_t) round(
                                convertMixxxToFfmpeg(l_fCurrentFFMPEGPosByte,
                                m_pFormatCtx->streams[m_iAudioStream]->time_base));
                            qDebug() << "ffmpeg: Warning: **** Packet PTS/DTS" 
                                << l_SPacket.pts<< "/" <<
                                l_SPacket.dts <<
                                " Time: " << currentFFMPEGPosSec << "/(" <<
                                currentFFMPEGPosByte << 
                                "|" << currentFFMPEGPosByte2 << 
                                " wrong calculation: "
                                <<  l_iWarning1 << 
                                " != " <<  l_iWarning1 - l_SPacket.pts;
                        }

                    }

                    if ( (((int64_t) round(convertMixxxToFfmpeg(
                           l_fCurrentFFMPEGPosByte,
                           m_pFormatCtx->streams[m_iAudioStream]->time_base))) - 
                           l_SPacket.pts) != 0) {
                        qDebug() << "ffmpeg: Warning: PTS Calculation error!";
                    }
                    // qDebug() << "PTS" << l_SPacket.pts << 
                    // "currentFFMPEGPosByte: " << currentFFMPEGPosByte << 
                    // " ffmpegsec " << currentFFMPEGPosSec << " Needed: " 
                    // << needed << " Got: " << readByteArray.size() 
                    // << "Now readed:" << readBytes;

                    // Remove these allways.. even if they aren't set
                    isFirstPacket = FALSE;
                    m_bIsSeeked = FALSE;

                    if (m_pResample->getBufferSize() > 0) {
                        readBuffer.write((const char *)m_pResample->getBuffer(),
                                         m_pResample->getBufferSize());
                        readBytes = m_pResample->getBufferSize();
                        m_pResample->removeBuffer();

                    } else {
                        readBuffer.write((const char *)
                                         (l_pFrame->data[0]), readBytes);
                    }

                    //av_free_packet(&l_SPacket);
                    av_free( l_SPacket.data );
                    l_SPacket.data = NULL;
                    l_SPacket.size = 0;
                    avcodec_get_frame_defaults(l_pFrame);

                } else {
                    qDebug() <<
                             "ffmpeg: libavcodec 'avcodec_decode_audio4' \
                              didn't succeed or frame not finished \
                              (File could also just end!)";
                }

            }

            if ( currentFFMPEGPosSec >= (toMixxPosSec + 0.02) ) {
                if ( readByteArray.size() >= (m_iOffset + copysize) ) {
                    m_bReadLoop = TRUE;
                }
            }


            //else
            //{
            //   qDebug() <<  "Someother packet possibly Video\n");
            //}


        } else {
            qDebug() << "ffmpeg: libavcodec 'av_read_frame' didn't succeed!";
            break;
        }


    }

    readBuffer.seek(0);
    readBuffer.seek(m_iOffset);
    readBuffer.read(pRead, copysize);

    m_strBuffer.clear();

    if ((m_iOffset + copysize) <= (unsigned) readByteArray.size()) {
        m_strBuffer = readByteArray.right(readByteArray.size() -
                                          (m_iOffset + copysize));
    } else {
        qDebug() << "ffmpeg: Too less bytes " << 
                 readByteArray.size() << "needed" <<
                 (m_iOffset + copysize);
    }

    m_iOffset = 0;

    av_free(l_pFrame);
#ifdef AV_CODEC_ID_NONE
    av_destruct_packet(&l_SPacket);
#endif

    m_bIsSeeked = FALSE;

    return size;

}

int SoundSourceFFmpeg::parseHeader() {
    qDebug() << "ffmpeg: SoundSourceFFmpeg::parseHeader" << m_qFilename;
#ifdef __WINDOWS__
    // From Tobias: A Utf-8 string did not work on my Windows XP (German edition)
    // If you try this conversion, f.isValid() will return false in many cases
    // and processTaglibFile() will fail
    //
    // The method toLocal8Bit() returns the local 8-bit representation of 
    // the string as a QByteArray. The returned byte array is undefined if the
    // string contains characters not supported
    // by the local 8-bit encoding.
    //
    // See https://ffmpeg.org/trac/ffmpeg/ticket/819 for relevant bug report.
    //
    QByteArray qBAFilename = m_qFilename.toLocal8Bit();
#else
    QByteArray qBAFilename = m_qFilename.toUtf8();
#endif

    AVFormatContext * FmtCtx = avformat_alloc_context();
    AVCodecContext * CodecCtx;
    AVDictionaryEntry *FmtTag = NULL;
    unsigned int i;

    //qDebug() << "ffmpeg: parsing file:" << qBAFilename.constData();
    if (avformat_open_input(&FmtCtx, qBAFilename.constData(), NULL, NULL) !=0) {
        qDebug() << "av_open_input_file: cannot open" << qBAFilename.constData();
        return ERR;
    }
    // Retrieve stream information
    if (avformat_find_stream_info(FmtCtx, NULL)<0) {
        qDebug() << "av_find_stream_info: Can't find metadata" <<
                 qBAFilename.constData();
        return ERR;
    }
    for (i=0; i<FmtCtx->nb_streams; i++)
        if (FmtCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            m_iAudioStream=i;
            break;
        }
    if (m_iAudioStream==-1) {
        qDebug() << "cannot find an audio stream: Can't find stream" <<
                 qBAFilename.constData();
        return ERR;
    }

    // Get a pointer to the codec context for the video stream
    CodecCtx=FmtCtx->streams[m_iAudioStream]->codec;

    //qDebug() << "ffmpeg: Parse HEADER [MP3,WMA]";

    while ((FmtTag = av_dict_get(FmtCtx->metadata, "", FmtTag,
                                 AV_DICT_IGNORE_SUFFIX))) {
        QString strValue (QString::fromUtf8 (FmtTag->value));

        if (!strncmp(FmtTag->key, "artist", 7)) {
            this->setArtist(strValue);
        } else if (!strncmp(FmtTag->key, "album", 5)) {
            this->setAlbum(strValue);
        } else if (!strncmp(FmtTag->key, "date", 4)) {
            this->setYear(strValue);
        } else if (!strncmp(FmtTag->key, "genre", 5)) {
            this->setGenre(strValue);
        } else if (!strncmp(FmtTag->key, "title", 5)) {
            this->setTitle(strValue);
        }


    }

    while ((FmtTag = av_dict_get(FmtCtx->streams[m_iAudioStream]->metadata, "",
                                 FmtTag, AV_DICT_IGNORE_SUFFIX))) {
        // Convert the value from UTF-8.
        QString strValue (QString::fromUtf8 (FmtTag->value));

        if (!strncmp(FmtTag->key, "ARTIST", 7)) {
            this->setArtist(strValue);
        } else if (!strncmp(FmtTag->key, "ALBUM", 5)) {
            this->setAlbum(strValue);
        } else if (!strncmp(FmtTag->key, "YEAR", 4)) {
            this->setYear(strValue);
        } else if (!strncmp(FmtTag->key, "GENRE", 5)) {
            this->setGenre(strValue);
        } else if (!strncmp(FmtTag->key, "TITLE", 5)) {
            this->setTitle(strValue);
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_TRACK_PEAK", 20)) {
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_TRACK_GAIN", 20)) {
            this->parseReplayGainString (strValue);
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_ALBUM_PEAK", 20)) {
        } else if (!strncmp(FmtTag->key, "REPLAYGAIN_ALBUM_GAIN", 20)) {
        }


    }

    this->setType(m_qFilename.section(".",-1).toLower());
    this->setDuration(FmtCtx->duration / AV_TIME_BASE);
    this->setBitrate((int)(CodecCtx->bit_rate / 1000));
    this->setSampleRate(CodecCtx->sample_rate);
    this->setChannels(CodecCtx->channels);
    avformat_close_input(&FmtCtx);
    return OK;
}

inline long unsigned SoundSourceFFmpeg::length() {
    return filelength;
}

QList<QString> SoundSourceFFmpeg::supportedFileExtensions() {
    QList<QString> list;
    AVInputFormat *l_SInputFmt  = NULL;

    while ((l_SInputFmt = av_iformat_next(l_SInputFmt))) {
        if (l_SInputFmt->name == NULL) {
            break;
        }

        qDebug() << l_SInputFmt->name;

        if (!strcmp(l_SInputFmt->name, "flac")) {
            list.push_back("flac");
            qDebug() << "FFPEG Decode: FLAC";
        } else if (!strcmp(l_SInputFmt->name, "ogg")) {
            list.push_back("ogg");
            qDebug() << "FFMPEG Decode: Ogg/Vorbis";
        } else if (!strcmp(l_SInputFmt->name, "mov,mp4,m4a,3gp,3g2,mj2")) {
            list.push_back("m4a");
            qDebug() << "FFMPEG Decode: Apple m4a";
        } else if (!strcmp(l_SInputFmt->name, "mp4")) {
            list.push_back("mp4");
            qDebug() << "FFMPEG Decode: Mp4";
        } else if (!strcmp(l_SInputFmt->name, "mp3")) {
            list.push_back("mp3");
            qDebug() << "FFMPEG Decode: Mp3";
        } else if (!strcmp(l_SInputFmt->name, "aac")) {
            list.push_back("aac");
            qDebug() << "FFMPEG Decode: Apple AAC";
        } else if (!strcmp(l_SInputFmt->name, "opus") ||
                   !strcmp(l_SInputFmt->name, "libopus")) {
            list.push_back("opus");
            qDebug() << "FFMPEG Decode: Opus";
        } else if (!strcmp(l_SInputFmt->name, "wma")) {
            list.push_back("xwma");
            qDebug() << "FFMPEG Decode: WMA";
        }
    }

    return list;
}
