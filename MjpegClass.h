/*******************************************************************************
 * JPEGDEC Wrapper Class
 *
 * Dependent libraries:
 * JPEGDEC: https://github.com/bitbank2/JPEGDEC.git
 ******************************************************************************/
#ifndef _MJPEGCLASS_H_
#define _MJPEGCLASS_H_

#define READ_BUFFER_SIZE 1024

#if defined(ESP32) || defined(ESP8266)
#include <FS.h>
#else
#include <SD.h>
#endif

#include <JPEGDEC.h>

class MjpegClass
{
public:
  MjpegClass() : _input(nullptr), _mjpeg_buf(nullptr), _mjpeg_buf_size(0),
                 _pfnDraw(nullptr), _read_buf(nullptr), _read_pos(0),
                 _buf_read(0), _mjpeg_buf_offset(0), _useBigEndian(false),
                 _x(0), _y(0), _widthLimit(0), _heightLimit(0), _scale(-1),
                 _oversize_frames(0), _forceNative(false), _frameWidth(0),
                 _frameHeight(0) {}

  ~MjpegClass() { free(_read_buf); }

  bool setup(Stream *input, uint8_t *mjpeg_buf, size_t mjpeg_buf_size,
             JPEG_DRAW_CALLBACK *pfnDraw, bool useBigEndian,
             int x, int y, int widthLimit, int heightLimit)
  {
    if (!input || !mjpeg_buf || mjpeg_buf_size < 4 || !pfnDraw ||
        widthLimit <= 0 || heightLimit <= 0) return false;
    _input = input;
    _mjpeg_buf = mjpeg_buf;
    _mjpeg_buf_size = mjpeg_buf_size;
    _pfnDraw = pfnDraw;
    _useBigEndian = useBigEndian;
    _x = x;
    _y = y;
    _widthLimit = widthLimit;
    _heightLimit = heightLimit;
    _read_pos = _buf_read = _mjpeg_buf_offset = 0;
    _scale = -1;
    _oversize_frames = 0;
    if (!_read_buf) _read_buf = static_cast<uint8_t *>(malloc(READ_BUFFER_SIZE));
    return _read_buf != nullptr;
  }

  // Returns true only for one complete, bounded JPEG frame. Invalid and
  // oversize frames are consumed, allowing a later valid frame to play.
  bool readMjpegBuf()
  {
    while (true)
    {
      uint8_t previous = 0, value = 0;
      bool havePrevious = false, foundSoi = false;
      while (readByte(value))
      {
        if (havePrevious && previous == 0xFF && value == 0xD8)
        {
          foundSoi = true;
          break;
        }
        previous = value;
        havePrevious = true;
      }
      if (!foundSoi) return false;
      _mjpeg_buf[0] = 0xFF;
      _mjpeg_buf[1] = 0xD8;
      _mjpeg_buf_offset = 2;
      if (readFrame()) return true;
      if (_read_pos >= _buf_read && !_input->available()) return false;
    }
  }

  void setForceNative(bool enabled) { _forceNative = enabled; }
  int getFrameWidth() const { return _frameWidth; }
  int getFrameHeight() const { return _frameHeight; }

  bool drawJpg()
  {
    if (!_mjpeg_buf || _mjpeg_buf_offset < 4 || !_pfnDraw) return false;
    // JPEGDEC openRAM() and decode() return non-zero on success.
    if (!_jpeg.openRAM(_mjpeg_buf, _mjpeg_buf_offset, _pfnDraw)) return false;
    bool decoded = false;
    const int w = _jpeg.getWidth();
    const int h = _jpeg.getHeight();
    _frameWidth = w;
    _frameHeight = h;
    if (w > 0 && h > 0)
    {
      int scale = 0, scaledW = w, scaledH = h;
      int maxMcus = _forceNative ? 64 : _widthLimit / 16;
      if (!_forceNative && h > _heightLimit * 4)
      {
        scale = JPEG_SCALE_EIGHTH;
        scaledW /= 8;
        scaledH /= 8;
        maxMcus = _widthLimit / 2;
      }
      else if (!_forceNative && h > _heightLimit * 2)
      {
        scale = JPEG_SCALE_QUARTER;
        scaledW /= 4;
        scaledH /= 4;
        maxMcus = _widthLimit / 4;
      }
      else if (!_forceNative && h > _heightLimit)
      {
        scale = JPEG_SCALE_HALF;
        scaledW /= 2;
        scaledH /= 2;
        maxMcus = _widthLimit / 8;
      }
      _jpeg.setMaxOutputSize(maxMcus > 0 ? maxMcus : 1);
      _jpeg.setPixelType(_useBigEndian ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);
      const int drawX = _forceNative ? 0 : (scaledW > _widthLimit ? 0 : (_widthLimit - scaledW) / 2);
      const int drawY = _forceNative ? 0 : (scaledH > _heightLimit ? 0 : (_heightLimit - scaledH) / 2);
      decoded = _jpeg.decode(drawX, drawY, scale) != 0;
    }
    _jpeg.close();
    return decoded;
  }

  uint32_t getOversizeFrameCount() const { return _oversize_frames; }

private:
  bool readByte(uint8_t &value)
  {
    if (_read_pos >= _buf_read)
    {
      _buf_read = _input->readBytes(_read_buf, READ_BUFFER_SIZE);
      _read_pos = 0;
      if (_buf_read == 0) return false;
    }
    value = _read_buf[_read_pos++];
    return true;
  }

  bool readFrame()
  {
    uint8_t previous = 0xD8, value = 0;
    bool overflow = false;
    while (readByte(value))
    {
      if (!overflow)
      {
        if (_mjpeg_buf_offset >= static_cast<int32_t>(_mjpeg_buf_size)) overflow = true;
        else _mjpeg_buf[_mjpeg_buf_offset++] = value;
      }
      if (previous == 0xFF && value == 0xD9)
      {
        if (!overflow && _mjpeg_buf_offset >= 4) return true;
        _oversize_frames++;
        return false;
      }
      previous = value;
    }
    return false;
  }

  Stream *_input;
  uint8_t *_mjpeg_buf;
  size_t _mjpeg_buf_size;
  JPEG_DRAW_CALLBACK *_pfnDraw;
  uint8_t *_read_buf;
  size_t _read_pos;
  size_t _buf_read;
  int32_t _mjpeg_buf_offset;
  bool _useBigEndian;
  int _x, _y, _widthLimit, _heightLimit, _scale;
  uint32_t _oversize_frames;
  bool _forceNative;
  int _frameWidth, _frameHeight;
  JPEGDEC _jpeg;
};

#endif // _MJPEGCLASS_H_
