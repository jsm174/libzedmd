#include "ZeDMDComm.h"

#include "miniz/miniz.h"
#include "komihash/komihash.h"

ZeDMDComm::ZeDMDComm()
{
   m_pThread = NULL;
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   m_pSerialPort = NULL;
#endif
}

ZeDMDComm::~ZeDMDComm()
{
   if (m_pThread) {
      m_pThread->join();

      delete m_pThread;
   }
}

void ZeDMDComm::SetLogMessageCallback(ZeDMD_LogMessageCallback callback, const void *userData)
{
   m_logMessageCallback = callback;
   m_logMessageUserData = userData;
}

void ZeDMDComm::LogMessage(const char *format, ...)
{
   if (!m_logMessageCallback)
      return;

   va_list args;
   va_start(args, format);
   (*(m_logMessageCallback))(format, args, m_logMessageUserData);
   va_end(args);
}

void ZeDMDComm::Run()
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   m_pThread = new std::thread([this]() {
      LogMessage("ZeDMDComm run thread starting");
      int8_t lastStreamId = -1;

      while (m_pSerialPort) {
         m_frameQueueMutex.lock();

         if (m_frames.empty()) {
            m_delayedFrameMutex.lock();
            if (m_delayedFrameReady) {
               while (m_delayedFrames.size() > 0) {
                  m_frames.push(m_delayedFrames.front());
                  m_delayedFrames.pop();
               }
               m_delayedFrameReady = false;
               m_delayedFrameMutex.unlock();
               m_frameQueueMutex.unlock();
               continue;
            }
            m_delayedFrameMutex.unlock();
            m_frameQueueMutex.unlock();

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
         }

         ZeDMDFrame frame = m_frames.front();
         m_frames.pop();
         if (frame.streamId > 0) {
            if (frame.streamId != lastStreamId) {
               lastStreamId = frame.streamId;
               m_frameCounter--;
            }
         }
         m_frameQueueMutex.unlock();

         bool success = StreamBytes(&frame);
         if (!success && frame.size < ZEDMD_COMM_FRAME_SIZE_COMMAND_LIMIT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            // Try to send the command again, in case the wait for the (R)eady signal ran into a timeout.
            success = StreamBytes(&frame);
         }

         if (frame.data)
            free(frame.data);
         
         if (!success)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }

      LogMessage("ZeDMDComm run thread finished");
   });
#endif
}

void ZeDMDComm::QueueCommand(char command, uint8_t *data, int size, int8_t streamId, bool delayed)
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   if (!m_pSerialPort)
      return;

   ZeDMDFrame frame = {0};
   frame.command = command;
   frame.size = size;
   frame.streamId = streamId;

   if (data && size > 0) {
      frame.data = (uint8_t *)malloc(size);
      memcpy(frame.data, data, size);
   }

   // delayed standard frame
   if (streamId == -1 && GetQueuedFramesCount() > ZEDMD_COMM_FRAME_QUEUE_SIZE_MAX) {
      m_delayedFrameMutex.lock();
      while (m_delayedFrames.size() > 0)
         m_delayedFrames.pop();

      m_delayedFrames.push(frame);
      m_delayedFrameReady = true;
      m_delayedFrameMutex.unlock();
   }
   // delayed streamed zones
   else if (streamId != -1 && delayed) {
      m_delayedFrameMutex.lock();
      m_delayedFrames.push(frame);
      m_delayedFrameMutex.unlock();
   }
   else {
      m_frameQueueMutex.lock();
      if (streamId == -1 || m_lastStreamId != streamId) {
         m_frameCounter++;
         m_lastStreamId = streamId;
      }
      m_frames.push(frame);
      m_frameQueueMutex.unlock();
   }
#endif
}

void ZeDMDComm::QueueCommand(char command, uint8_t value)
{
   QueueCommand(command, &value, 1);
}

void ZeDMDComm::QueueCommand(char command)
{
   QueueCommand(command, NULL, 0);
}

void ZeDMDComm::QueueCommand(char command, uint8_t *data, int size, uint16_t width, uint16_t height)
{
   uint8_t buffer[256 * 16 * 3 + 16]; 
   uint16_t bufferSize = 0;
   uint8_t idx = 0;
   uint8_t zone[16 * 8 * 3] = { 0 };

   if (++m_streamId > 64)
      m_streamId = 1;

   bool delayed = false;
   if (GetQueuedFramesCount() > ZEDMD_COMM_FRAME_QUEUE_SIZE_MAX) {
      delayed = true;
      m_delayedFrameMutex.lock();
      m_delayedFrameReady = false;
      while (m_delayedFrames.size() > 0)
         m_delayedFrames.pop();

      m_delayedFrameMutex.unlock();
   }

   for (uint16_t y = 0; y < height; y += m_zoneHeight) {       
       for (uint16_t x = 0; x < width; x += m_zoneWidth) {
         for (uint8_t z = 0; z < m_zoneHeight; z++)
            memcpy(&zone[z * m_zoneWidth * 3], &data[((y + z) * width + x) * 3], m_zoneWidth * 3);

         uint64_t hash = komihash(zone, m_zoneWidth * m_zoneHeight * 3, 0);
         if (hash != m_zoneHashes[idx]) {
            m_zoneHashes[idx] = hash;

            buffer[bufferSize++] = idx;
            memcpy(&buffer[bufferSize], zone, m_zoneWidth * m_zoneHeight * 3);
            bufferSize += m_zoneWidth * m_zoneHeight * 3;

            if (bufferSize >= (width * m_zoneHeight * 3 + 16)) {
               QueueCommand(command, buffer, bufferSize, m_streamId, delayed);
               bufferSize = 0;
            }
         }
         idx++;
      }
   }

   if (bufferSize > 0)
      QueueCommand(command, buffer, bufferSize, m_streamId, delayed);

   if (delayed) {
      m_delayedFrameMutex.lock();
      m_delayedFrameReady = true;
      m_delayedFrameMutex.unlock();
   }
}

uint8_t ZeDMDComm::GetQueuedFramesCount()
{
   uint8_t count = 0;
   m_frameQueueMutex.lock();
   count = m_frameCounter;
   m_frameQueueMutex.unlock();
   return count;
}

void ZeDMDComm::IgnoreDevice(const char* ignore_device)
{
   if (sizeof(ignore_device) < 32 && m_ignoredDevicesCounter < 10)
      strcpy(&m_ignoredDevices[m_ignoredDevicesCounter++][0], ignore_device);
}

void ZeDMDComm::SetDevice(const char* device)
{
   if (sizeof(device) < 32) 
      strcpy(&m_device[0], device);
}

bool ZeDMDComm::Connect()
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   if (m_device[0] != 0)
      return Connect(m_device);

   char szDevice[32];

   for (int i = 0; i < 7; i++) {
#ifdef __APPLE__
      sprintf(szDevice, "/dev/cu.usbserial-%04d", i);
#elif defined(_WIN32) || defined(_WIN64)
      sprintf(szDevice, "\\\\.\\COM%d", i + 1);
#else
      sprintf(szDevice, "/dev/ttyUSB%d", i);
#endif

      for (int j = 0; j < m_ignoredDevicesCounter; j++) {
         if (strcmp(szDevice, m_ignoredDevices[j]) == 0)
            continue;
      }

      if (Connect(szDevice))
         return true;
   }
#endif
   return false;
}

void ZeDMDComm::Disconnect()
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   if (!m_pSerialPort)
      return;

   Reset();

   sp_close(m_pSerialPort);
   m_pSerialPort = NULL;
#endif
}

bool ZeDMDComm::Connect(char *pDevice)
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   if (sp_get_port_by_name(pDevice, &m_pSerialPort) != SP_OK)
      return false;

   sp_open(m_pSerialPort, SP_MODE_READ_WRITE);
   
   sp_set_baudrate(m_pSerialPort, ZEDMD_COMM_BAUD_RATE);
   sp_set_bits(m_pSerialPort, 8);
   sp_set_parity(m_pSerialPort, SP_PARITY_NONE);
   sp_set_stopbits(m_pSerialPort, 1);

   Reset();

   uint8_t data[8] = {0};

   while (sp_input_waiting(m_pSerialPort) > 0)
      sp_blocking_read(m_pSerialPort, data, 8, ZEDMD_COMM_SERIAL_READ_TIMEOUT);

   std::this_thread::sleep_for(std::chrono::milliseconds(200));

   data[0] = ZEDMD_COMM_COMMAND::Handshake;
   sp_blocking_write(m_pSerialPort, (void*)CTRL_CHARS_HEADER, CTRL_CHARS_HEADER_SIZE, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
   sp_blocking_write(m_pSerialPort, (void*)data, 1, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
   
   std::this_thread::sleep_for(std::chrono::milliseconds(200));

   if (sp_blocking_read(m_pSerialPort, data, 8, 0)) {
      if (!memcmp(data, CTRL_CHARS_HEADER, 4)) {
         m_width = data[4] + data[5] * 256;
         m_height = data[6] + data[7] * 256;
         m_zoneWidth = m_width / 16;
         m_zoneHeight = m_height / 8;

         if (sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'R') {
            data[0] = ZEDMD_COMM_COMMAND::Compression;
            sp_blocking_write(m_pSerialPort, (void*)CTRL_CHARS_HEADER, 6, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
            sp_blocking_write(m_pSerialPort, (void*)data, 1, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));

            if (sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'A' &&
                sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'R') {

               data[0] = ZEDMD_COMM_COMMAND::Chunk;
               data[1] = ZEDMD_COMM_MAX_SERIAL_WRITE_AT_ONCE / 256;
               sp_blocking_write(m_pSerialPort, (void*)CTRL_CHARS_HEADER, 6, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
               sp_blocking_write(m_pSerialPort, (void*)data, 2, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
               std::this_thread::sleep_for(std::chrono::milliseconds(4));

               if (sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'A' &&
                  sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'R') {
                  data[0] = ZEDMD_COMM_COMMAND::EnableFlowControlV2;
                  sp_blocking_write(m_pSerialPort, (void*)CTRL_CHARS_HEADER, 6, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
                  sp_blocking_write(m_pSerialPort, (void*)data, 1, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
                  std::this_thread::sleep_for(std::chrono::milliseconds(4));

                  if (sp_blocking_read(m_pSerialPort, data, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT) && data[0] == 'A') {
                     m_flowControlCounter = 1;

                     if (pDevice) {
                        LogMessage("ZeDMD found: device=%s, width=%d, height=%d", pDevice, m_width, m_height);
                     }
                     else {
                        LogMessage("ZeDMD found: width=%d, height=%d", m_width, m_height);
                     }

                     return true;
                  }
               }
            }
         }
      }
   }

   Disconnect();
#endif

   return false;
}

void ZeDMDComm::Reset()
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   if (!m_pSerialPort)
      return;

   sp_set_dtr(m_pSerialPort, SP_DTR_OFF);
   sp_set_rts(m_pSerialPort, SP_RTS_ON);
   std::this_thread::sleep_for(std::chrono::milliseconds(200));

   sp_set_rts(m_pSerialPort, SP_RTS_OFF);
   sp_set_dtr(m_pSerialPort, SP_DTR_OFF);
   std::this_thread::sleep_for(std::chrono::milliseconds(200));

   sp_flush(m_pSerialPort, SP_BUF_BOTH);
   std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif
}

bool ZeDMDComm::StreamBytes(ZeDMDFrame *pFrame)
{
#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))
   uint8_t *data;
   int size;

   if (pFrame->size == 0) {
      size = CTRL_CHARS_HEADER_SIZE + 1;
      data = (uint8_t *)malloc(size);
      memcpy(data, CTRL_CHARS_HEADER, CTRL_CHARS_HEADER_SIZE);
      data[CTRL_CHARS_HEADER_SIZE] = pFrame->command;
   }
   else {
      mz_ulong compressedSize = mz_compressBound(pFrame->size);
      data = (uint8_t *)malloc(CTRL_CHARS_HEADER_SIZE + 3 + compressedSize);
      memcpy(data, CTRL_CHARS_HEADER, CTRL_CHARS_HEADER_SIZE);
      data[CTRL_CHARS_HEADER_SIZE] = pFrame->command;
      mz_compress(data + CTRL_CHARS_HEADER_SIZE + 3, &compressedSize, pFrame->data, pFrame->size);
      size = CTRL_CHARS_HEADER_SIZE + 3 + compressedSize;
      data[CTRL_CHARS_HEADER_SIZE + 1] = (uint8_t)(compressedSize >> 8 & 0xFF);
      data[CTRL_CHARS_HEADER_SIZE + 2] = (uint8_t)(compressedSize & 0xFF);
   }

   bool success = false;

   uint8_t flowControlCounter;
   do {
      // In case of a timeout, ReadByte() returns 0.
      sp_blocking_read(m_pSerialPort, &flowControlCounter, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT);
   } while (flowControlCounter != 0 && flowControlCounter != m_flowControlCounter);

   if (flowControlCounter == m_flowControlCounter) {
      int position = 0;
      success = true;

      while (position < size && success) {
         sp_blocking_write(m_pSerialPort, data + position, ((size - position) < ZEDMD_COMM_MAX_SERIAL_WRITE_AT_ONCE) ? (size - position) : ZEDMD_COMM_MAX_SERIAL_WRITE_AT_ONCE, ZEDMD_COMM_SERIAL_WRITE_TIMEOUT);
         
         uint8_t response;
         do {
            sp_blocking_read(m_pSerialPort, &response, 1, ZEDMD_COMM_SERIAL_READ_TIMEOUT);
         } while (response == flowControlCounter);

         if (response == 'A')
            position += ZEDMD_COMM_MAX_SERIAL_WRITE_AT_ONCE;
         else {
            success = false;
            LogMessage("Write bytes failure: response=%c", response);
         }
      }

      if (m_flowControlCounter < 32)
         m_flowControlCounter++;
      else
         m_flowControlCounter = 1;
   }
   else
   {
      LogMessage("No Ready Signal");
   }

   free(data);

   return success;
#else
   return false;
#endif
}

uint16_t ZeDMDComm::GetWidth()
{
   return m_width;
}

uint16_t ZeDMDComm::GetHeight()
{
   return m_height;
}
