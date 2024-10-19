#pragma once

#include <log4cxx/logger.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/propertyconfigurator.h>
#include <log4cxx/helpers/exception.h>

#pragma comment(lib, "log4cxx.lib")

namespace Mangos
{
   using namespace log4cxx;
   using namespace log4cxx::helpers;

   typedef bool (Logger::* Fct_Enabled)() const;
   typedef LevelPtr(*Fct_GetLevel)();

   class LogWriter
   {
   public:
      LogWriter(LoggerPtr const& logger, Fct_Enabled fct_enabled, Fct_GetLevel fct_getlevel, const char* file, int line, const char* function) :
         m_logger(logger),
         m_fct_enabled(fct_enabled),
         m_fct_getlevel(fct_getlevel),
         m_file(file),
         m_line(line),
         m_function(function)
      {
         size_t pos = m_file.find_last_of("/\\");

         if (pos != std::string::npos)
         {
            m_shortfile = m_file.substr(pos + 1);
         }
         else
         {
            m_shortfile = m_file;
         }
      }

      ~LogWriter()
      {
         if (LOG4CXX_UNLIKELY(((*m_logger).*m_fct_enabled)()))
         {
            ::log4cxx::spi::LocationInfo locationInfo(m_file.c_str(), m_shortfile.c_str(), m_function.c_str(), m_line);
            m_logger->forcedLog(m_fct_getlevel(), oss_.str(oss_), locationInfo);
         }
      }

      template<typename T>
      LogWriter& operator<<(T const& value)
      {
         oss_ << value;
         return *this;
      };

   private:
      LoggerPtr const& m_logger;
      Fct_Enabled m_fct_enabled;
      Fct_GetLevel m_fct_getlevel;

      std::string m_file;
      std::string m_shortfile;
      int m_line;
      std::string m_function;

      ::log4cxx::helpers::MessageBuffer oss_;
   };
}

#define CCLOG_TRACE(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isTraceEnabled, &::log4cxx::Level::getTrace, __FILE__, __LINE__, __FUNCTION__)
#define CCLOG_DEBUG(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isDebugEnabled, &::log4cxx::Level::getDebug, __FILE__, __LINE__, __FUNCTION__)
#define CCLOG_INFO(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isInfoEnabled, &::log4cxx::Level::getInfo, __FILE__, __LINE__, __FUNCTION__)
#define CCLOG_WARN(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isWarnEnabled, &::log4cxx::Level::getWarn, __FILE__, __LINE__, __FUNCTION__)
#define CCLOG_ERROR(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isErrorEnabled, &::log4cxx::Level::getError, __FILE__, __LINE__, __FUNCTION__)
#define CCLOG_FATAL(logger) Mangos::LogWriter(logger, &log4cxx::Logger::isFatalEnabled, &::log4cxx::Level::getFatal, __FILE__, __LINE__, __FUNCTION__)