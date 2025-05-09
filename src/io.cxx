#include <memory>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <array>
#include <stdexcept>

#ifdef _WIN32

#include <conio.h>
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define strcasecmp _stricmp
#define strdup _strdup
#define write _write
#define STDIN_FILENO 0

#include "windows.hxx"

#else /* _WIN32 */

#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#endif /* _WIN32 */

#include "io.hxx"
#include "conversion.hxx"
#include "escape.hxx"
#include "replxx.hxx"
#include "util.hxx"

using namespace std;

namespace replxx {

namespace tty {

bool is_a_tty( int fd_ ) {
	bool aTTY( isatty( fd_ ) != 0 );
#ifdef _WIN32
	do {
		if ( aTTY ) {
			break;
		}
		HANDLE h( (HANDLE)_get_osfhandle( fd_ ) );
		if ( h == INVALID_HANDLE_VALUE ) {
			break;
		}
		DWORD st( 0 );
		if ( ! GetConsoleMode( h, &st ) ) {
			break;
		}
		aTTY = true;
	} while ( false );
#endif
	return ( aTTY );
}

bool in( is_a_tty( 0 ) );
bool out( is_a_tty( 1 ) );

}

Terminal::Terminal( void )
#ifdef _WIN32
	: _consoleOut()
	, _consoleIn()
	, _oldMode()
	, _oldDisplayAttribute()
	, _inputCodePage( GetConsoleCP() )
	, _outputCodePage( GetConsoleOutputCP() )
	, _interrupt( INVALID_HANDLE_VALUE )
	, _events()
#else
	: _origTermios()
	, _interrupt()
#endif
	, _rawMode( false ) {
#ifdef _WIN32
	_interrupt = CreateEvent( nullptr, true, false, TEXT( "replxx_interrupt_event" ) );
#else
	static_cast<void>( ::pipe( _interrupt ) == 0 );
#endif
}

Terminal::~Terminal( void ) {
	if ( _rawMode ) {
		disable_raw_mode();
	}
#ifdef _WIN32
	CloseHandle( _interrupt );
#else
	static_cast<void>( ::close( _interrupt[0] ) == 0 );
	static_cast<void>( ::close( _interrupt[1] ) == 0 );
#endif
}

void Terminal::write32( char32_t const* text32, int len32 ) {
	int len8 = 4 * len32 + 1;
	unique_ptr<char[]> text8(new char[len8]);
	int count8 = 0;

	copyString32to8(text8.get(), len8, text32, len32, &count8);
	int nWritten( 0 );
#ifdef _WIN32
	nWritten = win_write( text8.get(), count8 );
#else
	nWritten = write( 1, text8.get(), count8 );
#endif
	if ( nWritten != count8 ) {
		throw std::runtime_error( "write failed" );
	}
	return;
}

void Terminal::write8( char const* data_, int size_ ) {
#ifdef _WIN32
	int nWritten( win_write( data_, size_ ) );
#else
	int nWritten( write( 1, data_, size_ ) );
#endif
	if ( nWritten != size_ ) {
		throw std::runtime_error( "write failed" );
	}
	return;
}

int Terminal::get_screen_columns( void ) {
	int cols( 0 );
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO inf;
	GetConsoleScreenBufferInfo( _consoleOut, &inf );
	cols = inf.dwSize.X;
#else
	struct winsize ws;
	cols = ( ioctl( 1, TIOCGWINSZ, &ws ) == -1 ) ? 80 : ws.ws_col;
#endif
	// cols is 0 in certain circumstances like inside debugger, which creates
	// further issues
	return ( cols > 0 ) ? cols : 80;
}

int Terminal::get_screen_rows( void ) {
	int rows;
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO inf;
	GetConsoleScreenBufferInfo( _consoleOut, &inf );
	rows = 1 + inf.srWindow.Bottom - inf.srWindow.Top;
#else
	struct winsize ws;
	rows = (ioctl(1, TIOCGWINSZ, &ws) == -1) ? 24 : ws.ws_row;
#endif
	return (rows > 0) ? rows : 24;
}

namespace {
inline int notty( void ) {
	errno = ENOTTY;
	return ( -1 );
}
}

int Terminal::enable_raw_mode( void ) {
	if ( ! _rawMode ) {
#ifdef _WIN32
		_consoleIn = GetStdHandle( STD_INPUT_HANDLE );
		_consoleOut = GetStdHandle( STD_OUTPUT_HANDLE );
		SetConsoleCP( 65001 );
		SetConsoleOutputCP( 65001 );
		GetConsoleMode( _consoleIn, &_oldMode );
		SetConsoleMode(
			_consoleIn,
			_oldMode & ~( ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT )
		);
#else
		struct termios raw;

		if ( ! tty::in ) {
			return ( notty() );
		}
		if ( tcgetattr( 0, &_origTermios ) == -1 ) {
			return ( notty() );
		}

		raw = _origTermios; /* modify the original mode */
		/* input modes: no break, no CR to NL, no parity check, no strip char,
		 * no start/stop output control. */
		raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
		/* output modes - disable post processing */
		// this is wrong, we don't want raw output, it turns newlines into straight
		// linefeeds
		// raw.c_oflag &= ~(OPOST);
		/* control modes - set 8 bit chars */
		raw.c_cflag |= (CS8);
		/* local modes - echoing off, canonical off, no extended functions,
		 * no signal chars (^Z,^C) */
		raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
		/* control chars - set return condition: min number of bytes and timer.
		 * We want read to return every single byte, without timeout. */
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

		/* put terminal in raw mode after flushing */
		if ( tcsetattr(0, TCSADRAIN, &raw) < 0 ) {
			return ( notty() );
		}
#endif
		_rawMode = true;
	}
	return 0;
}

void Terminal::disable_raw_mode(void) {
	if ( _rawMode ) {
#ifdef _WIN32
		SetConsoleMode( _consoleIn, _oldMode );
		SetConsoleCP( _inputCodePage );
		SetConsoleOutputCP( _outputCodePage );
		_consoleIn = 0;
		_consoleOut = 0;
#else
		if ( tcsetattr( 0, TCSADRAIN, &_origTermios ) == -1 ) {
			return;
		}
#endif
		_rawMode = false;
	}
}

#ifndef _WIN32

/**
 * Read a UTF-8 sequence from the non-Windows keyboard and return the Unicode
 * (char32_t) character it
 * encodes
 *
 * @return	char32_t Unicode character
 */
char32_t read_unicode_character(void) {
	static uchar8_t utf8String[5];
	static size_t utf8Count = 0;
	while (true) {
		uchar8_t c;

		/* Continue reading if interrupted by signal. */
		ssize_t nread;
		do {
			nread = read(0, &c, 1);
		} while ((nread == -1) && (errno == EINTR));

		if (nread <= 0) return 0;
		if (c <= 0x7F || locale::is8BitEncoding) {	// short circuit ASCII
			utf8Count = 0;
			return c;
		} else if (utf8Count < sizeof(utf8String) - 1) {
			utf8String[utf8Count++] = c;
			utf8String[utf8Count] = 0;
			char32_t unicodeChar[2];
			int ucharCount( 0 );
			ConversionResult res = copyString8to32(unicodeChar, 2, ucharCount, utf8String);
			if (res == conversionOK && ucharCount) {
				utf8Count = 0;
				return unicodeChar[0];
			}
		} else {
			utf8Count = 0;	// this shouldn't happen: got four bytes but no UTF-8 character
		}
	}
}

#endif	// #ifndef _WIN32

void beep() {
	fprintf(stderr, "\x7");	// ctrl-G == bell/beep
	fflush(stderr);
}

// replxx_read_char -- read a keystroke or keychord from the keyboard, and translate it
// into an encoded "keystroke".	When convenient, extended keys are translated into their
// simpler Emacs keystrokes, so an unmodified "left arrow" becomes Ctrl-B.
//
// A return value of zero means "no input available", and a return value of -1
// means "invalid key".
//
char32_t Terminal::read_char( void ) {
	char32_t c( 0 );
#ifdef _WIN32
	INPUT_RECORD rec;
	DWORD count;
	char32_t modifierKeys = 0;
	bool escSeen = false;
	int highSurrogate( 0 );
	while (true) {
		ReadConsoleInputW( _consoleIn, &rec, 1, &count );
#if __REPLXX_DEBUG__	// helper for debugging keystrokes, display info in the debug "Output"
		// window in the debugger
		{
			if ( rec.EventType == KEY_EVENT ) {
				//if ( rec.Event.KeyEvent.uChar.UnicodeChar ) {
				char buf[1024];
				sprintf(
					buf,
					"Unicode character 0x%04X, repeat count %d, virtual keycode 0x%04X, "
					"virtual scancode 0x%04X, key %s%s%s%s%s\n",
					rec.Event.KeyEvent.uChar.UnicodeChar,
					rec.Event.KeyEvent.wRepeatCount,
					rec.Event.KeyEvent.wVirtualKeyCode,
					rec.Event.KeyEvent.wVirtualScanCode,
					rec.Event.KeyEvent.bKeyDown ? "down" : "up",
					(rec.Event.KeyEvent.dwControlKeyState & LEFT_CTRL_PRESSED) ? " L-Ctrl" : "",
					(rec.Event.KeyEvent.dwControlKeyState & RIGHT_CTRL_PRESSED) ? " R-Ctrl" : "",
					(rec.Event.KeyEvent.dwControlKeyState & LEFT_ALT_PRESSED) ? " L-Alt" : "",
					(rec.Event.KeyEvent.dwControlKeyState & RIGHT_ALT_PRESSED) ? " R-Alt" : ""
				);
				OutputDebugStringA( buf );
				//}
			}
		}
#endif
		if (rec.EventType != KEY_EVENT) {
			continue;
		}
		// Windows provides for entry of characters that are not on your keyboard by
		// sending the
		// Unicode characters as a "key up" with virtual keycode 0x12 (VK_MENU ==
		// Alt key) ...
		// accept these characters, otherwise only process characters on "key down"
		if (!rec.Event.KeyEvent.bKeyDown &&
				rec.Event.KeyEvent.wVirtualKeyCode != VK_MENU) {
			continue;
		}
		modifierKeys = 0;
		// AltGr is encoded as ( LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED ), so don't
		// treat this
		// combination as either CTRL or META we just turn off those two bits, so it
		// is still
		// possible to combine CTRL and/or META with an AltGr key by using
		// right-Ctrl and/or
		// left-Alt
		if ((rec.Event.KeyEvent.dwControlKeyState &
				 (LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED)) ==
				(LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED)) {
			rec.Event.KeyEvent.dwControlKeyState &=
					~(LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
		}
		if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED ) ) {
			modifierKeys |= Replxx::KEY::BASE_CONTROL;
		}
		if ( rec.Event.KeyEvent.dwControlKeyState & ( RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED ) ) {
			modifierKeys |= Replxx::KEY::BASE_META;
		}
		if (escSeen) {
			modifierKeys |= Replxx::KEY::BASE_META;
		}
		int key( rec.Event.KeyEvent.uChar.UnicodeChar );
		if ( key == 0 ) {
			switch (rec.Event.KeyEvent.wVirtualKeyCode) {
				case VK_LEFT:
					return modifierKeys | Replxx::KEY::LEFT;
				case VK_RIGHT:
					return modifierKeys | Replxx::KEY::RIGHT;
				case VK_UP:
					return modifierKeys | Replxx::KEY::UP;
				case VK_DOWN:
					return modifierKeys | Replxx::KEY::DOWN;
				case VK_DELETE:
					return modifierKeys | Replxx::KEY::DELETE;
				case VK_HOME:
					return modifierKeys | Replxx::KEY::HOME;
				case VK_END:
					return modifierKeys | Replxx::KEY::END;
				case VK_PRIOR:
					return modifierKeys | Replxx::KEY::PAGE_UP;
				case VK_NEXT:
					return modifierKeys | Replxx::KEY::PAGE_DOWN;
				case VK_F1:
					return modifierKeys | Replxx::KEY::F1;
				case VK_F2:
					return modifierKeys | Replxx::KEY::F2;
				case VK_F3:
					return modifierKeys | Replxx::KEY::F3;
				case VK_F4:
					return modifierKeys | Replxx::KEY::F4;
				case VK_F5:
					return modifierKeys | Replxx::KEY::F5;
				case VK_F6:
					return modifierKeys | Replxx::KEY::F6;
				case VK_F7:
					return modifierKeys | Replxx::KEY::F7;
				case VK_F8:
					return modifierKeys | Replxx::KEY::F8;
				case VK_F9:
					return modifierKeys | Replxx::KEY::F9;
				case VK_F10:
					return modifierKeys | Replxx::KEY::F10;
				case VK_F11:
					return modifierKeys | Replxx::KEY::F11;
				case VK_F12:
					return modifierKeys | Replxx::KEY::F12;
				default:
					continue; // in raw mode, ReadConsoleInput shows shift, ctrl - ignore them
			}
		} else if ( key == Replxx::KEY::ESCAPE ) { // ESC, set flag for later
			escSeen = true;
			continue;
		} else if ( ( key >= 0xD800 ) && ( key <= 0xDBFF ) ) {
			highSurrogate = key - 0xD800;
			continue;
		} else {
			// we got a real character, return it
			if ( ( key >= 0xDC00 ) && ( key <= 0xDFFF ) ) {
				key -= 0xDC00;
				key |= ( highSurrogate << 10 );
				key += 0x10000;
			}
			if ( is_control_code( key ) ) {
				key += 0x40;
				modifierKeys |= Replxx::KEY::BASE_CONTROL;
			}
			key |= modifierKeys;
			highSurrogate = 0;
			c = key;
			break;
		}
	}

#else
	c = read_unicode_character();
	if (c == 0) {
		return 0;
	}

// If _DEBUG_LINUX_KEYBOARD is set, then ctrl-^ puts us into a keyboard
// debugging mode
// where we print out decimal and decoded values for whatever the "terminal"
// program
// gives us on different keystrokes.	Hit ctrl-C to exit this mode.
//
#ifdef __REPLXX_DEBUG__
	if (c == ctrlChar('^')) {	// ctrl-^, special debug mode, prints all keys hit,
														 // ctrl-C to get out
		printf(
				"\nEntering keyboard debugging mode (on ctrl-^), press ctrl-C to exit "
				"this mode\n");
		while (true) {
			unsigned char keys[10];
			int ret = read(0, keys, 10);

			if (ret <= 0) {
				printf("\nret: %d\n", ret);
			}
			for (int i = 0; i < ret; ++i) {
				char32_t key = static_cast<char32_t>(keys[i]);
				char* friendlyTextPtr;
				char friendlyTextBuf[10];
				const char* prefixText = (key < 0x80) ? "" : "0x80+";
				char32_t keyCopy = (key < 0x80) ? key : key - 0x80;
				if (keyCopy >= '!' && keyCopy <= '~') {	// printable
					friendlyTextBuf[0] = '\'';
					friendlyTextBuf[1] = keyCopy;
					friendlyTextBuf[2] = '\'';
					friendlyTextBuf[3] = 0;
					friendlyTextPtr = friendlyTextBuf;
				} else if (keyCopy == ' ') {
					friendlyTextPtr = const_cast<char*>("space");
				} else if (keyCopy == 27) {
					friendlyTextPtr = const_cast<char*>("ESC");
				} else if (keyCopy == 0) {
					friendlyTextPtr = const_cast<char*>("NUL");
				} else if (keyCopy == 127) {
					friendlyTextPtr = const_cast<char*>("DEL");
				} else {
					friendlyTextBuf[0] = '^';
					friendlyTextBuf[1] = keyCopy + 0x40;
					friendlyTextBuf[2] = 0;
					friendlyTextPtr = friendlyTextBuf;
				}
				printf("%d x%02X (%s%s)	", key, key, prefixText, friendlyTextPtr);
			}
			printf("\x1b[1G\n");	// go to first column of new line

			// drop out of this loop on ctrl-C
			if (keys[0] == ctrlChar('C')) {
				printf("Leaving keyboard debugging mode (on ctrl-C)\n");
				fflush(stdout);
				return -2;
			}
		}
	}
#endif // __REPLXX_DEBUG__

	c = EscapeSequenceProcessing::doDispatch(c);
	if ( is_control_code( c ) ) {
		c = Replxx::KEY::control( c + 0x40 );
	}
#endif // #_WIN32
	return ( c );
}

Terminal::EVENT_TYPE Terminal::wait_for_input( void ) {
#ifdef _WIN32
	std::array<HANDLE,2> handles = { _consoleIn, _interrupt };
	while ( true ) {
		DWORD event( WaitForMultipleObjects( handles.size (), handles.data(), false, INFINITE ) );
		switch ( event ) {
			case ( WAIT_OBJECT_0 + 0 ): {
				// peek events that will be skipped
				INPUT_RECORD rec;
				DWORD count;
				PeekConsoleInputW( _consoleIn, &rec, 1, &count );

				if (
					( rec.EventType != KEY_EVENT )
					|| ( !rec.Event.KeyEvent.bKeyDown && ( rec.Event.KeyEvent.wVirtualKeyCode != VK_MENU ) )
				) {
					// read the event to unsignal the handle
					ReadConsoleInputW( _consoleIn, &rec, 1, &count );
					continue;
				}

				return ( EVENT_TYPE::KEY_PRESS );
			}
			case ( WAIT_OBJECT_0 + 1 ): {
				ResetEvent( _interrupt );
				if ( _events.empty() ) {
					continue;
				}
				EVENT_TYPE eventType( _events.front() );
				_events.pop_front();
				return ( eventType );
			}
		}
	}
#else
	fd_set fdSet;
	int nfds( max( _interrupt[0], _interrupt[1] ) + 1 );
	while ( true ) {
		FD_ZERO( &fdSet );
		FD_SET( 0, &fdSet );
		FD_SET( _interrupt[0], &fdSet );
		int err( select( nfds, &fdSet, nullptr, nullptr, nullptr ) );
		if ( ( err == -1 ) && ( errno == EINTR ) ) {
			continue;
		}
		if ( FD_ISSET( _interrupt[0], &fdSet ) ) {
			char data( 0 );
			static_cast<void>( read( _interrupt[0], &data, 1 ) == 1 );
			if ( data == 'k' ) {
				return ( EVENT_TYPE::KEY_PRESS );
			}
			if ( data == 'm' ) {
				return ( EVENT_TYPE::MESSAGE );
			}
		}
		if ( FD_ISSET( 0, &fdSet ) ) {
			return ( EVENT_TYPE::KEY_PRESS );
		}
	}
#endif
}

void Terminal::notify_event( EVENT_TYPE eventType_ ) {
#ifdef _WIN32
	_events.push_back( eventType_ );
	SetEvent( _interrupt );
#else
	char data( eventType_ == EVENT_TYPE::KEY_PRESS ? 'k' : 'm' );
	static_cast<void>( write( _interrupt[1], &data, 1 ) == 1 );
#endif
}

/**
 * Clear the screen ONLY (no redisplay of anything)
 */
void Terminal::clear_screen( CLEAR_SCREEN clearScreen_ ) {
#ifdef _WIN32
	COORD coord = { 0, 0 };
	CONSOLE_SCREEN_BUFFER_INFO inf;
	bool toEnd( clearScreen_ == CLEAR_SCREEN::TO_END );
	GetConsoleScreenBufferInfo( _consoleOut, &inf );
	if ( ! toEnd ) {
		SetConsoleCursorPosition( _consoleOut, coord );
	} else {
		coord = inf.dwCursorPosition;
	}
	DWORD nWritten( 0 );
	DWORD toWrite(
		toEnd
			? ( inf.dwSize.Y - inf.dwCursorPosition.Y ) * inf.dwSize.X - inf.dwCursorPosition.X
			: inf.dwSize.X * inf.dwSize.Y
	);
	FillConsoleOutputCharacterA( _consoleOut, ' ', toWrite, coord, &nWritten );
#else
	if ( clearScreen_ == CLEAR_SCREEN::WHOLE ) {
		char const clearCode[] = "\033c\033[H\033[2J\033[0m";
		static_cast<void>( write(1, clearCode, sizeof ( clearCode ) - 1) >= 0 );
	} else {
		char const clearCode[] = "\033[J";
		static_cast<void>( write(1, clearCode, sizeof ( clearCode ) - 1) >= 0 );
	}
#endif
}

void Terminal::jump_cursor( int xPos_, int yOffset_ ) {
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO inf;
	GetConsoleScreenBufferInfo( _consoleOut, &inf );
	inf.dwCursorPosition.X = xPos_;
	inf.dwCursorPosition.Y += yOffset_;
	SetConsoleCursorPosition( _consoleOut, inf.dwCursorPosition );
#else
	char seq[64];
	if ( yOffset_ != 0 ) { // move the cursor up as required
		snprintf( seq, sizeof seq, "\033[%d%c", abs( yOffset_ ), yOffset_ > 0 ? 'B' : 'A' );
		write8( seq, strlen( seq ) );
	}
	// position at the end of the prompt, clear to end of screen
	snprintf(
		seq, sizeof seq, "\033[%dG",
		xPos_ + 1 /* 1-based on VT100 */
	);
	write8( seq, strlen( seq ) );
#endif
}

}

