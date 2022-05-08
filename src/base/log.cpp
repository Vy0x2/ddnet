#include "logger.h"

#include "color.h"
#include "system.h"

#include <atomic>
#include <cstdio>

#if defined(CONF_FAMILY_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501 /* required for mingw to get getaddrinfo to work */
#include <windows.h>
#endif

extern "C" {

std::atomic<ILogger *> global_logger = nullptr;
thread_local ILogger *scope_logger = nullptr;
thread_local bool in_logger = false;

void log_set_global_logger(ILogger *logger)
{
	ILogger *null = nullptr;
	if(!global_logger.compare_exchange_strong(null, logger, std::memory_order_acq_rel))
	{
		dbg_assert(false, "global logger has already been set and can only be set once");
	}
	atexit(log_global_logger_finish);
}

void log_global_logger_finish()
{
	global_logger.load(std::memory_order_acquire)->GlobalFinish();
}

void log_set_global_logger_default()
{
	std::unique_ptr<ILogger> logger;
#if defined(CONF_PLATFORM_ANDROID)
	logger = log_logger_android();
#else
	logger = log_logger_stdout();
#endif
	log_set_global_logger(logger.release());
}

ILogger *log_get_scope_logger()
{
	if(!scope_logger)
	{
		scope_logger = global_logger.load(std::memory_order_acquire);
	}
	return scope_logger;
}

void log_set_scope_logger(ILogger *logger)
{
	scope_logger = logger;
	if(!scope_logger)
	{
		scope_logger = global_logger.load(std::memory_order_acquire);
	}
}

void log_log_impl(LEVEL level, bool have_color, LOG_COLOR color, const char *sys, const char *fmt, va_list args)
{
	// Make sure we're not logging recursively.
	if(in_logger)
	{
		return;
	}
	in_logger = true;
	if(!scope_logger)
	{
		scope_logger = global_logger.load(std::memory_order_acquire);
	}
	if(!scope_logger)
	{
		in_logger = false;
		return;
	}

	CLogMessage Msg;
	Msg.m_Level = level;
	Msg.m_HaveColor = have_color;
	Msg.m_Color = color;
	str_timestamp_format(Msg.m_aTimestamp, sizeof(Msg.m_aTimestamp), FORMAT_SPACE);
	Msg.m_TimestampLength = str_length(Msg.m_aTimestamp);
	str_copy(Msg.m_aSystem, sys, sizeof(Msg.m_aSystem));
	Msg.m_SystemLength = str_length(Msg.m_aSystem);

	// TODO: Add level?
	str_format(Msg.m_aLine, sizeof(Msg.m_aLine), "[%s][%s]: ", Msg.m_aTimestamp, Msg.m_aSystem);
	Msg.m_LineMessageOffset = str_length(Msg.m_aLine);

	char *pMessage = Msg.m_aLine + Msg.m_LineMessageOffset;
	int MessageSize = sizeof(Msg.m_aLine) - Msg.m_LineMessageOffset;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(pMessage, MessageSize, fmt, args);
#else
	vsnprintf(pMessage, MessageSize, fmt, args);
#endif
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
	Msg.m_LineLength = str_length(Msg.m_aLine);
	scope_logger->Log(&Msg);
	in_logger = false;
}

void log_log_v(LEVEL level, const char *sys, const char *fmt, va_list args)
{
	log_log_impl(level, false, LOG_COLOR{0, 0, 0}, sys, fmt, args);
}

void log_log(LEVEL level, const char *sys, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_log_impl(level, false, LOG_COLOR{0, 0, 0}, sys, fmt, args);
	va_end(args);
}

void log_log_color_v(LEVEL level, LOG_COLOR color, const char *sys, const char *fmt, va_list args)
{
	log_log_impl(level, true, color, sys, fmt, args);
}

void log_log_color(LEVEL level, LOG_COLOR color, const char *sys, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_log_impl(level, true, color, sys, fmt, args);
	va_end(args);
}
}

#if defined(CONF_PLATFORM_ANDROID)
class CLoggerAndroid : public ILogger
{
public:
	void Log(const CLogMessage *pMessage) override
	{
		int AndroidLevel;
		switch(Level)
		{
		case LEVEL_TRACE: AndroidLevel = ANDROID_LOG_VERBOSE; break;
		case LEVEL_DEBUG: AndroidLevel = ANDROID_LOG_DEBUG; break;
		case LEVEL_INFO: AndroidLevel = ANDROID_LOG_INFO; break;
		case LEVEL_WARN: AndroidLevel = ANDROID_LOG_WARN; break;
		case LEVEL_ERROR: AndroidLevel = ANDROID_LOG_ERROR; break;
		}
		__android_log_write(AndroidLevel, pMessage->m_aSystem, pMessage->Message());
	}
};
std::unique_ptr<ILogger> log_logger_android()
{
	return std::unique_ptr<ILogger>(new CLoggerAndroid());
}
#else
std::unique_ptr<ILogger> log_logger_android()
{
	dbg_assert(0, "Android logger on non-Android");
	return nullptr;
}
#endif

class CLoggerCollection : public ILogger
{
	std::vector<std::shared_ptr<ILogger>> m_apLoggers;

public:
	CLoggerCollection(std::vector<std::shared_ptr<ILogger>> &&apLoggers) :
		m_apLoggers(std::move(apLoggers))
	{
	}
	void Log(const CLogMessage *pMessage) override
	{
		for(auto &pLogger : m_apLoggers)
		{
			pLogger->Log(pMessage);
		}
	}
	void GlobalFinish() override
	{
		for(auto &pLogger : m_apLoggers)
		{
			pLogger->GlobalFinish();
		}
	}
};

std::unique_ptr<ILogger> log_logger_collection(std::vector<std::shared_ptr<ILogger>> &&loggers)
{
	return std::unique_ptr<ILogger>(new CLoggerCollection(std::move(loggers)));
}

class CLoggerAsync : public ILogger
{
	ASYNCIO *m_pAio;
	bool m_AnsiTruecolor;
	bool m_Close;

public:
	CLoggerAsync(IOHANDLE File, bool AnsiTruecolor, bool Close) :
		m_pAio(aio_new(File)),
		m_AnsiTruecolor(AnsiTruecolor),
		m_Close(Close)
	{
	}
	void Log(const CLogMessage *pMessage) override
	{
		aio_lock(m_pAio);
		if(m_AnsiTruecolor)
		{
			// https://en.wikipedia.org/w/index.php?title=ANSI_escape_code&oldid=1077146479#24-bit
			char aAnsi[32];
			if(pMessage->m_HaveColor)
			{
				str_format(aAnsi, sizeof(aAnsi),
					"\x1b[38;2;%d;%d;%dm",
					pMessage->m_Color.r,
					pMessage->m_Color.g,
					pMessage->m_Color.b);
			}
			else
			{
				str_copy(aAnsi, "\x1b[39m", sizeof(aAnsi));
			}
			aio_write_unlocked(m_pAio, aAnsi, str_length(aAnsi));
		}
		aio_write_unlocked(m_pAio, pMessage->m_aLine, pMessage->m_LineLength);
		aio_write_newline_unlocked(m_pAio);
		aio_unlock(m_pAio);
	}
	~CLoggerAsync()
	{
		if(m_Close)
		{
			aio_close(m_pAio);
		}
		aio_wait(m_pAio);
		aio_free(m_pAio);
	}
	void GlobalFinish() override
	{
		if(m_Close)
		{
			aio_close(m_pAio);
		}
		aio_wait(m_pAio);
	}
};

std::unique_ptr<ILogger> log_logger_file(IOHANDLE logfile)
{
	return std::unique_ptr<ILogger>(new CLoggerAsync(logfile, false, true));
}

#if defined(CONF_FAMILY_WINDOWS)
static int color_hsv_to_windows_console_color(const ColorHSVA &Hsv)
{
	int h = Hsv.h * 255.0f;
	int s = Hsv.s * 255.0f;
	int v = Hsv.v * 255.0f;
	if(s >= 0 && s <= 10)
	{
		if(v <= 150)
			return 8;
		return 15;
	}
	else if(h >= 0 && h < 15)
		return 12;
	else if(h >= 15 && h < 30)
		return 6;
	else if(h >= 30 && h < 60)
		return 14;
	else if(h >= 60 && h < 110)
		return 10;
	else if(h >= 110 && h < 140)
		return 11;
	else if(h >= 140 && h < 170)
		return 9;
	else if(h >= 170 && h < 195)
		return 5;
	else if(h >= 195 && h < 240)
		return 13;
	else if(h >= 240)
		return 12;
	else
		return 15;
}

class CWindowsConsoleLogger : public ILogger
{
public:
	void Log(const CLogMessage *pMessage) override
	{
		wchar_t *pWide = (wchar_t *)malloc((pMessage->m_LineLength + 1) * sizeof(*pWide));
		const char *p = pMessage->m_aLine;
		int WLen = 0;

		mem_zero(pWide, pMessage->m_LineLength * sizeof(*pWide));

		for(int Codepoint = 0; (Codepoint = str_utf8_decode(&p)); WLen++)
		{
			char aU16[4] = {0};

			if(Codepoint < 0)
			{
				free(pWide);
				return;
			}

			if(str_utf16le_encode(aU16, Codepoint) != 2)
			{
				free(pWide);
				return;
			}

			mem_copy(&pWide[WLen], aU16, 2);
		}
		pWide[WLen] = '\n';
		HANDLE pConsole = GetStdHandle(STD_OUTPUT_HANDLE);
		int Color = 15;
		if(pMessage->m_HaveColor)
		{
			ColorRGBA Rgba(1.0, 1.0, 1.0, 1.0);
			Rgba.r = pMessage->m_Color.r / 255.0;
			Rgba.g = pMessage->m_Color.g / 255.0;
			Rgba.b = pMessage->m_Color.b / 255.0;
			Color = color_hsv_to_windows_console_color(color_cast<ColorHSVA>(Rgba));
		}
		SetConsoleTextAttribute(pConsole, Color);
		WriteConsoleW(pConsole, pWide, WLen + 1, NULL, NULL);
		free(pWide);
	}
};
#endif

std::unique_ptr<ILogger> log_logger_stdout()
{
#if !defined(CONF_FAMILY_WINDOWS)
	// TODO: Only enable true color when COLORTERM contains "truecolor".
	// https://github.com/termstandard/colors/tree/65bf0cd1ece7c15fa33a17c17528b02c99f1ae0b#checking-for-colorterm
	return std::unique_ptr<ILogger>(new CLoggerAsync(io_stdout(), getenv("NO_COLOR") == nullptr, false));
#else
	return std::unique_ptr<ILogger>(new CWindowsConsoleLogger());
#endif
}

#if defined(CONF_FAMILY_WINDOWS)
class CLoggerWindowsDebugger : public ILogger
{
public:
	void Log(const CLogMessage *pMessage) override
	{
		WCHAR aWBuffer[4096];
		MultiByteToWideChar(CP_UTF8, 0, pMessage->m_aLine, -1, aWBuffer, sizeof(aWBuffer) / sizeof(WCHAR));
		OutputDebugStringW(aWBuffer);
	}
};
std::unique_ptr<ILogger> log_logger_windows_debugger()
{
	return std::unique_ptr<ILogger>(new CLoggerWindowsDebugger());
}
#else
std::unique_ptr<ILogger> log_logger_windows_debugger()
{
	dbg_assert(0, "Windows Debug logger on non-Windows");
	return nullptr;
}
#endif

void CFutureLogger::Set(std::unique_ptr<ILogger> &&pLogger)
{
	ILogger *null = nullptr;
	m_PendingLock.lock();
	ILogger *pLoggerRaw = pLogger.release();
	if(!m_pLogger.compare_exchange_strong(null, pLoggerRaw, std::memory_order_acq_rel))
	{
		dbg_assert(false, "future logger has already been set and can only be set once");
	}
	for(const auto &Pending : m_aPending)
	{
		pLoggerRaw->Log(&Pending);
	}
	m_aPending.clear();
	m_aPending.shrink_to_fit();
	m_PendingLock.unlock();
}

void CFutureLogger::Log(const CLogMessage *pMessage)
{
	ILogger *pLogger = m_pLogger.load(std::memory_order_acquire);
	if(pLogger)
	{
		pLogger->Log(pMessage);
		return;
	}
	m_PendingLock.lock();
	m_aPending.push_back(*pMessage);
	m_PendingLock.unlock();
}

void CFutureLogger::GlobalFinish()
{
	ILogger *pLogger = m_pLogger.load(std::memory_order_acquire);
	if(pLogger)
	{
		pLogger->GlobalFinish();
	}
}
