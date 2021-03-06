/* GNUPLOT - qt_term.cpp */

/*[
 * Copyright 2009   Jérôme Lodewyck
 *
 * Permission to use, copy, and distribute this software and its
 * documentation for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.
 *
 * Permission to modify the software is granted, but not the right to
 * distribute the complete modified source code.  Modifications are to
 * be distributed as patches to the released version.  Permission to
 * distribute binaries produced by compiling modified sources is granted,
 * provided you
 *   1. distribute the corresponding source modifications from the
 *    released version in the form of a patch file along with the binaries,
 *   2. add special version identification to distinguish your version
 *    in addition to the base release version number,
 *   3. provide your name and address as the primary contact for the
 *    support of your modified version, and
 *   4. retain our contact information in regard to use of the base
 *    software.
 * Permission to distribute the released version of the source code along
 * with corresponding source modifications in the form of a patch file is
 * granted with same provisions 2 through 4 for binary distributions.
 *
 * This software is provided "as is" without express or implied warranty
 * to the extent permitted by applicable law.
 *
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 or later (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above. If you wish to allow
 * use of your version of this file only under the terms of the GPL and not
 * to allow others to use your version of this file under the above gnuplot
 * license, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the GPL. If you do not
 * delete the provisions above, a recipient may use your version of this file
 * under either the GPL or the gnuplot license.
]*/

/*
 *  Thomas Bleher -  October 2013
 *  The Qt terminal is changed to only create its data on initialization
 *  (to avoid the Static Initialization Order Fiasco) and to destroy its data
 *  in a gnuplot atexit handler.
 */

#include <QtCore>
#include <QtGui>
#include <QtNetwork>

extern "C" {
	#include "plot.h"      // for interactive
	#include "term_api.h"  // for stdfn.h, JUSTIFY, encoding, *term definition, color.h term_interlock
	#include "mouse.h"     // for do_event declaration
	#include "getcolor.h"  // for rgb functions
	#include "command.h"   // for paused_for_mouse, PAUSE_BUTTON1 and friends
	#include "util.h"      // for int_error
	#include "alloc.h"     // for gp_alloc
	#include "parse.h"     // for real_expression
	#include "axis.h"
	#include <signal.h>
	double removeDockIcon();
}

#include "qt_term.h"
#include "QtGnuplotEvent.h"
#include "QtGnuplotApplication.h"
#include "qt_conversion.cpp"

void qt_atexit();

static int argc = 1;
static char empty = '\0';
static char* emptyp = &empty;

struct QtGnuplotState {
    /// @todo per-window options

    // Create a QApplication without event loop for QObject's that need it, namely font handling
    // A better strategy would be to transfer the font handling to the QtGnuplotWidget, but it would require
    // some synchronization between the widget and the gnuplot process.
    QApplication application;

    /*-------------------------------------------------------
     * State variables
     *-------------------------------------------------------*/

    bool gnuplot_qtStarted;
    int  currentFontSize;
    QString currentFontName;
    QString localServerName;
    QTextCodec* codec;

    // Events coming from gnuplot are processed by this file
    // and send to the GUI by a QDataStream writing on a
    // QByteArray sent trough a QLocalSocket (a cross-platform IPC protocol)
    QLocalSocket socket;
    QByteArray   outBuffer;
    QDataStream  out;

    bool       enhancedSymbol;
    QString    enhancedFontName;
    double     enhancedFontSize;
    double     enhancedBase;
    bool       enhancedWidthFlag;
    bool       enhancedShowFlag;
    int        enhancedOverprint;
    QByteArray enhancedText;

    /// Constructor
    QtGnuplotState()
        : application(argc, &emptyp)

        , gnuplot_qtStarted(false)
        , currentFontSize()
        , currentFontName()
        , localServerName()
        , codec(QTextCodec::codecForLocale())

        , socket()
        , outBuffer()
        , out(&outBuffer, QIODevice::WriteOnly)

        , enhancedSymbol(false)
        , enhancedFontName()
        , enhancedFontSize()
        , enhancedBase()
        , enhancedWidthFlag()
        , enhancedShowFlag()
        , enhancedOverprint()
        , enhancedText()
    {
    }

};

static QtGnuplotState* qt = NULL;

static const int qt_oversampling = 10;
static const double qt_oversamplingF = double(qt_oversampling);

/*-------------------------------------------------------
 * Terminal options with default values
 *-------------------------------------------------------*/
static int  qt_optionWindowId = 0;
static bool qt_optionEnhanced = true;
static bool qt_optionPersist  = false;
static bool qt_optionRaise    = true;
static bool qt_optionCtrl     = false;
static bool qt_optionDash     = false;
static int  qt_optionWidth    = 640;
static int  qt_optionHeight   = 480;
static int  qt_optionFontSize = 9;
/* Encapsulates all Qt options that have a constructor and destructor. */
struct QtOption {
    QtOption()
        : FontName("Sans")
    {}

    QString FontName;
    QString Title;
    QString Widget;
};
static QtOption* qt_option = NULL;

static void ensureOptionsCreated()
{
	if (!qt_option)
	    qt_option = new QtOption();
}


static bool qt_setSize   = true;
static int  qt_setWidth  = qt_optionWidth;
static int  qt_setHeight = qt_optionHeight;

/* ------------------------------------------------------
 * Helpers
 * ------------------------------------------------------*/

// Convert gnuplot coordinates into floating point term coordinates
QPointF qt_termCoordF(unsigned int x, unsigned int y)
{
	return QPointF(double(x)/qt_oversamplingF, double(term->ymax - y)/qt_oversamplingF);
}

// The same, but with coordinates clipped to the nearest pixel
QPoint qt_termCoord(unsigned int x, unsigned int y)
{
	return QPoint(qRound(double(x)/qt_oversamplingF), qRound(double(term->ymax - y)/qt_oversamplingF));
}

// Start the GUI application
void execGnuplotQt()
{
	// Fork the GUI and exec the gnuplot_qt program in the child process
	pid_t pid = fork();
	if (pid < 0)
		fprintf(stderr, "Forking error\n");
	else if (pid == 0) // Child: start the GUI
	{
		// Make sure the forked copy doesn't trash the history file 
		cancel_history();

		// Start the gnuplot_qt program
		QString filename = getenv("GNUPLOT_DRIVER_DIR");
		if (filename.isEmpty())
			filename = QT_DRIVER_DIR;
		filename += "/gnuplot_qt";

		execlp(filename.toUtf8().data(), "gnuplot_qt", (char*)NULL);
		fprintf(stderr, "Expected Qt driver: %s\n", filename.toUtf8().data());
		perror("Exec failed");
		gp_exit(EXIT_FAILURE);
	}
	qt->localServerName = "qtgnuplot" + QString::number(pid);
	qt->gnuplot_qtStarted = true;
}

/*-------------------------------------------------------
 * Communication gnuplot -> terminal
 *-------------------------------------------------------*/

void qt_flushOutBuffer()
{
	if (!qt)
		return;

	// Write the block size at the beginning of the bock
	QDataStream sizeStream(&qt->socket);
	sizeStream << (quint32)(qt->outBuffer.size());
	// Write the block to the QLocalSocket
	qt->socket.write(qt->outBuffer);
	// waitForBytesWritten(-1) is supposed implement this loop, but it does not seem to work !
	// update: seems to work with Qt 4.5
	while (qt->socket.bytesToWrite() > 0)
	{
		qt->socket.flush();
		qt->socket.waitForBytesWritten(-1);
	}
	// Reset the buffer
	qt->out.device()->seek(0);
	qt->outBuffer.clear();
}

// Helper function called by qt_connectToServer()
void qt_connectToServer(const QString& server, bool retry = true)
{
	ensureOptionsCreated();
	bool connectToWidget = (server != qt->localServerName);

	// The QLocalSocket::waitForConnected does not respect the time out argument when the
	// gnuplot_qt application is not yet started. To wait for it, we need to implement the timeout ourselves
	QDateTime timeout = QDateTime::currentDateTime().addMSecs(1000);
	do
	{
		qt->socket.connectToServer(server);
		qt->socket.waitForConnected(200);
		// TODO: yield CPU ?
	}
	while((qt->socket.state() != QLocalSocket::ConnectedState) && (QDateTime::currentDateTime() < timeout));

	// Still not connected...
	if ((qt->socket.state() != QLocalSocket::ConnectedState) && retry)
	{
		// The widget could not be reached: start a gnuplot_qt program which will create a QtGnuplotApplication
		if (connectToWidget)
		{
			qDebug() << "Could not connect to widget" << qt_option->Widget << ". Starting a QtGnuplotApplication";
			qt_option->Widget = QString();
			qt_connectToServer(qt->localServerName);
		}
		// The gnuplot_qt program could not be reached: try to start a new one
		else
		{
			qDebug() << "Could not connect gnuplot_qt" << qt_option->Widget << ". Starting a new one";
			execGnuplotQt();
			qt_connectToServer(qt->localServerName, false);
		}
	}
}

// Called before a plot to connect to the terminal window, if needed
void qt_connectToServer()
{
	if (!qt)
		return;
	ensureOptionsCreated();

	// Determine to which server we should connect
	bool connectToWidget = !qt_option->Widget.isEmpty();
	QString server = connectToWidget ? qt_option->Widget : qt->localServerName;

	if (qt->socket.state() == QLocalSocket::ConnectedState)
	{
		// Check if we are already connected to the correct server
		if (qt->socket.serverName() == server)
			return;

		// Otherwise disconnect
		qt->socket.disconnectFromServer();
		while (qt->socket.state() == QLocalSocket::ConnectedState)
			qt->socket.waitForDisconnected(1000);
	}

	// Start the gnuplot_qt helper program if not already started
	if (!connectToWidget && !qt->gnuplot_qtStarted)
		execGnuplotQt();

	// Connect to the server, or local server if not available.
	qt_connectToServer(server);
}

/*-------------------------------------------------------
 * Communication terminal -> gnuplot
 *-------------------------------------------------------*/

bool qt_processTermEvent(gp_event_t* event)
{
	// Intercepts resize event
	if (event->type == GE_fontprops)
	{
		qt_setSize   = true;
		qt_setWidth  = event->mx;
		qt_setHeight = event->my;
	}
	// Scale mouse events
	else
	{
		event->mx *= qt_oversampling;
		event->my = (qt_setHeight - event->my)*qt_oversampling;
	}

	// Send the event to gnuplot core
	do_event(event);
	// Process pause_for_mouse
	if ((event->type == GE_buttonrelease) && (paused_for_mouse & PAUSE_CLICK))
	{
		int button = event->par1;
		if (((button == 1) && (paused_for_mouse & PAUSE_BUTTON1)) ||
		    ((button == 2) && (paused_for_mouse & PAUSE_BUTTON2)) ||
		    ((button == 3) && (paused_for_mouse & PAUSE_BUTTON3)))
			paused_for_mouse = 0;
		if (paused_for_mouse == 0)
			return true;
	}
	if ((event->type == GE_keypress) && (paused_for_mouse & PAUSE_KEYSTROKE) && (event->par1 > '\0'))
	{
		paused_for_mouse = 0;
		return true;
	}

	return false;
}

/* ------------------------------------------------------
 * Functions called by gnuplot
 * ------------------------------------------------------*/

// Called before first plot after a set term command.
void qt_init()
{
    if (qt)
		return;
    ensureOptionsCreated();

    qt = new QtGnuplotState();

	// If we are not connecting to an existing QtGnuplotWidget, start a QtGnuplotApplication
	if (qt_option->Widget.isEmpty())
		execGnuplotQt();


#ifdef Q_WS_MAC
	// Don't display this application in the MAC OS X dock
	removeDockIcon();
#endif

	// The creation of a QApplication mangled our locale settings
#ifdef HAVE_LOCALE_H
	setlocale(LC_NUMERIC, "C");
	setlocale(LC_TIME, current_locale);
#endif

	qt->out.setVersion(QDataStream::Qt_4_4);
	term_interlock = (void *)qt_init;
	gp_atexit(qt_atexit);
}

// Called just before a plot is going to be displayed.
void qt_graphics()
{
	ensureOptionsCreated();
	qt->out << GEDesactivate;
	qt_flushOutBuffer();
	qt_connectToServer();

	// Set text encoding
	if (!(qt->codec = qt_encodingToCodec(encoding)))
		qt->codec = QTextCodec::codecForLocale();

	// Set font
	qt->currentFontSize = qt_optionFontSize;
	qt->currentFontName = qt_option->FontName;

	// Set plot metrics
	QFontMetrics metrics(QFont(qt->currentFontName, qt->currentFontSize));
	term->v_char = qt_oversampling * (metrics.ascent() + metrics.descent());
	term->h_char = qt_oversampling * metrics.width("0123456789")/10.;
	term->v_tic = (unsigned int) (term->v_char/2.5);
	term->h_tic = (unsigned int) (term->v_char/2.5);
	if (qt_setSize)
	{
		term->xmax = qt_oversampling*qt_setWidth;
		term->ymax = qt_oversampling*qt_setHeight;
		qt_setSize = false;
	}

	// Initialize window
	qt->out << GESetCurrentWindow << qt_optionWindowId;
	qt->out << GEInitWindow;
	qt->out << GEActivate;
	qt->out << GETitle << qt_option->Title;
	qt->out << GESetCtrl << qt_optionCtrl;
	qt->out << GESetWidgetSize << QSize(term->xmax, term->ymax)/qt_oversampling;
	// Initialize the scene
	qt->out << GESetSceneSize << QSize(term->xmax, term->ymax)/qt_oversampling;
	qt->out << GEClear;
	// Initialize the font
	qt->out << GESetFont << qt->currentFontName << qt->currentFontSize;
}

// Called after plotting is done
void qt_text()
{
	if (qt_optionRaise)
		qt->out << GERaise;
	qt->out << GEDone;
	qt_flushOutBuffer();
}

void qt_text_wrapper()
{
	// Remember scale to update the status bar while the plot is inactive
	qt->out << GEScale;

	const int axis_order[4] = {FIRST_X_AXIS, FIRST_Y_AXIS, SECOND_X_AXIS, SECOND_Y_AXIS};

	for (int i = 0; i < 4; i++)
	{
		qt->out << (axis_array[axis_order[i]].ticmode != NO_TICS); // Axis active or not
		qt->out << axis_array[axis_order[i]].min;
		double lower = double(axis_array[axis_order[i]].term_lower);
		double scale = double(axis_array[axis_order[i]].term_scale);
		// Reverse the y axis
		if (i % 2)
		{
			lower = term->ymax - lower;
			scale *= -1;
		}
		qt->out << lower/qt_oversamplingF << scale/qt_oversamplingF;
		qt->out << (axis_array[axis_order[i]].log ? axis_array[axis_order[i]].log_base : 0.);
	}

	qt_text();
}

void qt_reset()
{
	/// @todo
}

void qt_move(unsigned int x, unsigned int y)
{
	qt->out << GEMove << qt_termCoordF(x, y);
}

void qt_vector(unsigned int x, unsigned int y)
{
	qt->out << GEVector << qt_termCoordF(x, y);
}

void qt_enhanced_flush()
{
	qt->out << GEEnhancedFlush << qt->enhancedFontName << qt->enhancedFontSize
	       << qt->enhancedBase << qt->enhancedWidthFlag << qt->enhancedShowFlag
	       << qt->enhancedOverprint << qt->codec->toUnicode(qt->enhancedText);
	qt->enhancedText.clear();
}

void qt_enhanced_writec(int c)
{
	if (qt->enhancedSymbol)
		qt->enhancedText.append(qt->codec->fromUnicode(qt_symbolToUnicode(c)));
	else
		qt->enhancedText.append(char(c));
}

void qt_enhanced_open(char* fontname, double fontsize, double base, TBOOLEAN widthflag, TBOOLEAN showflag, int overprint)
{
	qt->enhancedFontName = fontname;
	if (qt->enhancedFontName.toLower() == "symbol")
	{
		qt->enhancedSymbol = true;
		qt->enhancedFontName = "Sans";
	}
	else
		qt->enhancedSymbol = false;

	qt->enhancedFontSize  = fontsize;
	qt->enhancedBase      = base;
	qt->enhancedWidthFlag = widthflag;
	qt->enhancedShowFlag  = showflag;
	qt->enhancedOverprint = overprint;
}

void qt_put_text(unsigned int x, unsigned int y, const char* string)
{
	// if ignore_enhanced_text is set, draw with the normal routine.
	// This is meant to avoid enhanced syntax when the enhanced mode is on
	if (!qt_optionEnhanced || ignore_enhanced_text)
	{
		/// @todo Symbol font to unicode
		/// @todo bold, italic
		qt->out << GEPutText << qt_termCoord(x, y) << qt->codec->toUnicode(string);
		return;
	}

	// Uses enhanced_recursion() to analyse the string to print.
	// enhanced_recursion() calls _enhanced_open() to initialize the text drawing,
	// then it calls _enhanced_writec() which buffers the characters to draw,
	// and finally _enhanced_flush() to draw the buffer with the correct justification.

	// set up the global variables needed by enhanced_recursion()
	enhanced_fontscale = 1.0;
	strncpy(enhanced_escape_format, "%c", sizeof(enhanced_escape_format));

	// Set the recursion going. We say to keep going until a closing brace, but
	// we don't really expect to find one.  If the return value is not the nul-
	// terminator of the string, that can only mean that we did find an unmatched
	// closing brace in the string. We increment past it (else we get stuck
	// in an infinite loop) and try again.
	while (*(string = enhanced_recursion((char*)string, TRUE, qt->currentFontName.toUtf8().data(),
			qt->currentFontSize, 0.0, TRUE, TRUE, 0)))
	{
		qt_enhanced_flush();
		enh_err_check(string); // we can only get here if *str == '}'
		if (!*++string)
			break; // end of string
		// else carry on and process the rest of the string
	}

	qt->out << GEEnhancedFinish << qt_termCoord(x, y);
}

void qt_linetype(int lt)
{
	if (lt <= LT_NODRAW)
		lt = LT_NODRAW; // background color

	if (lt == -1)
		qt->out << GEPenStyle << Qt::DotLine;
	else if (qt_optionDash && lt > 0) {
		Qt::PenStyle style;
		style =
			(lt%4 == 1) ? Qt::DashLine :
			(lt%4 == 2) ? Qt::DashDotLine :
			(lt%4 == 3) ? Qt::DashDotDotLine :
			              Qt::SolidLine ;
		qt->out << GEPenStyle << style;
		}
	else
		qt->out << GEPenStyle << Qt::SolidLine;

	if ((lt-1) == LT_BACKGROUND) {
		/* FIXME: Add parameter to this API to set the background color from the gnuplot end */
		qt->out << GEBackgroundColor;
	} else
		qt->out << GEPenColor << qt_colorList[lt % 9 + 3];
}

int qt_set_font (const char* font)
{
	ensureOptionsCreated();
	int  qt_previousFontSize = qt->currentFontSize;
	QString qt_previousFontName = qt->currentFontName;

	if (font && (*font))
	{
		QStringList list = QString(font).split(',');
		if (list.size() > 0)
			qt->currentFontName = list[0];
		if (list.size() > 1)
			qt->currentFontSize = list[1].toInt();
	} else {
		qt->currentFontSize = qt_optionFontSize;
		qt->currentFontName = qt_option->FontName;
	}

	if (qt->currentFontName.isEmpty())
		qt->currentFontName = qt_option->FontName;

	if (qt->currentFontSize <= 0)
		qt->currentFontSize = qt_optionFontSize;

	/* Optimize by leaving early if there is no change */
	if (qt->currentFontSize == qt_previousFontSize
	&&  qt->currentFontName == qt->currentFontName) {
		return 1;
	}

	qt->out << GESetFont << qt->currentFontName << qt->currentFontSize;

	/* Update the font size as seen by the core gnuplot code */
	QFontMetrics metrics(QFont(qt->currentFontName, qt->currentFontSize));
	term->v_char = qt_oversampling * (metrics.ascent() + metrics.descent());
	term->h_char = qt_oversampling * metrics.width("0123456789")/10.;

	return 1;
}

int qt_justify_text(enum JUSTIFY mode)
{
	if (mode == LEFT)
		qt->out << GETextAlignment << Qt::AlignLeft;
	else if (mode == RIGHT)
		qt->out << GETextAlignment << Qt::AlignRight;
	else if (mode == CENTRE)
		qt->out << GETextAlignment << Qt::AlignCenter;

	return 1; // We can justify
}

void qt_point(unsigned int x, unsigned int y, int pointstyle)
{
	qt->out << GEPoint << qt_termCoordF(x, y) << pointstyle;
}

void qt_pointsize(double ptsize)
{
	if (ptsize < 0.) ptsize = 1.; // same behaviour as x11 terminal
	qt->out << GEPointSize << ptsize;
}

void qt_linewidth(double lw)
{
	qt->out << GELineWidth << lw;
}

int qt_text_angle(int angle)
{
	qt->out << GETextAngle << double(angle);
	return 1; // 1 means we can rotate
}

void qt_fillbox(int style, unsigned int x, unsigned int y, unsigned int width, unsigned int height)
{
	qt->out << GEBrushStyle << style;
	qt->out << GEFillBox << QRect(qt_termCoord(x, y + height), QSize(width, height)/qt_oversampling);
}

int qt_make_palette(t_sm_palette* palette)
{
	return 0; // We can do continuous colors
}

void qt_set_color(t_colorspec* colorspec)
{
	if (colorspec->type == TC_LT) {
		if (colorspec->lt <= LT_NODRAW)
			qt->out << GEBackgroundColor;
		else
			qt->out << GEPenColor << qt_colorList[colorspec->lt % 9 + 3];
	}
	else if (colorspec->type == TC_FRAC)
	{
		rgb_color rgb;
		rgb1maxcolors_from_gray(colorspec->value, &rgb);
		QColor color;
		color.setRgbF(rgb.r, rgb.g, rgb.b);
		qt->out << GEPenColor << color;
	}
	else if (colorspec->type == TC_RGB) {
		QColor color = QRgb(colorspec->lt);
		int alpha = (colorspec->lt >> 24) & 0xff;
		if (alpha > 0)
			color.setAlpha(255-alpha);
		qt->out << GEPenColor << color;
	}
}

void qt_filled_polygon(int n, gpiPoint *corners)
{
	QPolygonF polygon;
	for (int i = 0; i < n; i++)
		polygon << qt_termCoordF(corners[i].x, corners[i].y);

	qt->out << GEBrushStyle << corners->style;
	qt->out << GEFilledPolygon << polygon;
}

void qt_image(unsigned int M, unsigned int N, coordval* image, gpiPoint* corner, t_imagecolor color_mode)
{
	QImage qimage = qt_imageToQImage(M, N, image, color_mode);
	qt->out << GEImage;
	for (int i = 0; i < 4; i++)
		qt->out << qt_termCoord(corner[i].x, corner[i].y);
	qt->out << qimage;
}

#ifdef USE_MOUSE

// Display temporary text, after
// erasing any temporary text displayed previously at this location.
// The int determines where: 0=statusline, 1,2: at corners of zoom
// box, with \r separating text above and below the point.
void qt_put_tmptext(int n, const char str[])
{
    if (!qt)
        return;

	if (n == 0)
		qt->out << GEStatusText << QString(str);
	else if (n == 1)
		qt->out << GEZoomStart << QString(str);
	else if (n == 2)
		qt->out << GEZoomStop << QString(str);

	qt_flushOutBuffer();
}

void qt_set_cursor(int c, int x, int y)
{
    if (!qt)
        return;

	// Cancel zoombox when Echap is pressed
	if (c == 0)
		qt->out << GEZoomStop << QString();

	if (c == -4)
		qt->out << GELineTo << false;
	else if (c == -3)
		qt->out << GELineTo << true;
	else if (c == -2) // warp the pointer to the given position
		qt->out << GEWrapCursor << qt_termCoord(x, y);
	else if (c == -1) // start zooming
		qt->out << GECursor << Qt::SizeFDiagCursor;
	else if (c ==  1) // Rotation
		qt->out << GECursor << Qt::ClosedHandCursor;
	else if (c ==  2) // Rescale
		qt->out << GECursor << Qt::SizeAllCursor;
	else if (c ==  3) // Zoom
		qt->out << GECursor << Qt::SizeFDiagCursor;
	else
		qt->out << GECursor << Qt::CrossCursor;

	qt_flushOutBuffer();
}

void qt_set_ruler(int x, int y)
{
	qt->out << GERuler << qt_termCoord(x, y);
	qt_flushOutBuffer();
}

void qt_set_clipboard(const char s[])
{
	qt->out << GECopyClipboard << s;
	qt_flushOutBuffer();
}
#endif // USE_MOUSE

int qt_waitforinput(int options)
{
#ifdef USE_MOUSE
	fd_set read_fds;
	struct timeval one_msec;
	int stdin_fd  = fileno(stdin);
	int socket_fd = qt ? qt->socket.socketDescriptor() : -1;

	if (!qt || (socket_fd < 0) || (qt->socket.state() != QLocalSocket::ConnectedState))
		return (options == TERM_ONLY_CHECK_MOUSING) ? '\0' : getchar();

	// Gnuplot event loop
	do
	{
		// Watch file descriptors
		struct timeval *timeout = NULL;
		FD_ZERO(&read_fds);
		FD_SET(socket_fd, &read_fds);
		if (!paused_for_mouse)
			FD_SET(stdin_fd, &read_fds);

		// When taking input from the console, we are willing to wait
		// here until the next character is typed.  But if input is from
		// a script we just want to check for hotkeys or mouse input and
		// then leave again without waiting on stdin.
		if (options == TERM_ONLY_CHECK_MOUSING) {
			timeout = &one_msec;
			one_msec.tv_sec = 0;
			one_msec.tv_usec = TERM_EVENT_POLL_TIMEOUT;
		}

		// Wait for input
		if (select(socket_fd+1, &read_fds, NULL, NULL, timeout) < 0)
		{
			// Display the error message except when Ctrl + C is pressed
			if (errno != 4)
				fprintf(stderr, "Qt terminal communication error: select() error %i %s\n", errno, strerror(errno));
			break;
		}

		// Terminal event coming
		if (FD_ISSET(socket_fd, &read_fds))
		{
			qt->socket.waitForReadyRead(-1);
			// Temporary event for mouse move events. If several consecutive move events
			// are received, only transmit the last one.
			gp_event_t tempEvent;
			tempEvent.type = -1;
			while (qt->socket.bytesAvailable() >= (int)sizeof(gp_event_t))
			{
				struct gp_event_t event;
				qt->socket.read((char*) &event, sizeof(gp_event_t));
				// Delay move events
				if (event.type == GE_motion)
					tempEvent = event;
				// Other events. Replay the last move event if present
				else
				{
					if (tempEvent.type == GE_motion)
					{
						qt_processTermEvent(&tempEvent);
						tempEvent.type = -1;
					}
					if (qt_processTermEvent(&event))
						return '\0'; // exit from paused_for_mouse
				}
			}
			// Replay move event
			if (tempEvent.type == GE_motion)
				qt_processTermEvent(&tempEvent);
		}

		else if (options == TERM_ONLY_CHECK_MOUSING) {
			return '\0';
		}
	} while (paused_for_mouse || !FD_ISSET(stdin_fd, &read_fds));
#endif
	return getchar();
}

/*-------------------------------------------------------
 * Misc
 *-------------------------------------------------------*/

void qt_atexit()
{
	if (qt_optionPersist || persist_cl)
	{
		qt->out << GEDesactivate;
		qt->out << GEPersist;
	}
	else
		qt->out << GEExit;
	qt_flushOutBuffer();
        
        delete qt;
        qt = NULL;

	delete qt_option;
	qt_option = NULL;
}

/*-------------------------------------------------------
 * Term options
 *-------------------------------------------------------*/

enum QT_id {
	QT_WIDGET,
	QT_FONT,
	QT_ENHANCED,
	QT_NOENHANCED,
	QT_SIZE,
	QT_PERSIST,
	QT_NOPERSIST,
	QT_RAISE,
	QT_NORAISE,
	QT_CTRL,
	QT_NOCTRL,
	QT_TITLE,
	QT_CLOSE,
	QT_DASH,
	QT_DASHLENGTH,
	QT_SOLID,
	QT_OTHER
};

static struct gen_table qt_opts[] = {
	{"$widget",     QT_WIDGET},
	{"font",        QT_FONT},
	{"enh$anced",   QT_ENHANCED},
	{"noenh$anced", QT_NOENHANCED},
	{"s$ize",       QT_SIZE},
	{"per$sist",    QT_PERSIST},
	{"noper$sist",  QT_NOPERSIST},
	{"rai$se",      QT_RAISE},
	{"norai$se",    QT_NORAISE},
	{"ct$rlq",      QT_CTRL},
	{"noct$rlq",    QT_NOCTRL},
	{"ti$tle",      QT_TITLE},
	{"cl$ose",      QT_CLOSE},
	{"dash$ed",	QT_DASH},
	{"dashl$ength",	QT_DASHLENGTH},
	{"dl",		QT_DASHLENGTH},
	{"solid",	QT_SOLID},
	{NULL,          QT_OTHER}
};

// Called when terminal type is selected.
// This procedure should parse options on the command line.
// A list of the currently selected options should be stored in term_options[],
// in a form suitable for use with the set term command.
// term_options[] is used by the save command.  Use options_null() if no options are available." *
void qt_options()
{
	ensureOptionsCreated();
	char *s = NULL;
	QString fontSettings;
	bool duplication = false;
	bool set_enhanced = false, set_font = false;
	bool set_persist = false, set_number = false;
	bool set_raise = false, set_ctrl = false;
	bool set_title = false, set_close = false;
	bool set_size = false;
	bool set_widget = false;
	bool set_dash = false;

	if (term_interlock != NULL && term_interlock != (void *)qt_init) {
		term = NULL;
		int_error(NO_CARET, "The qt terminal cannot be used in a wxt session");
	}

#define SETCHECKDUP(x) { c_token++; if (x) duplication = true; x = true; }

	while (!END_OF_COMMAND)
	{
		FPRINTF((stderr, "processing token\n"));
		switch (lookup_table(&qt_opts[0], c_token)) {
		case QT_WIDGET:
			SETCHECKDUP(set_widget);
			if (!(s = try_to_get_string()))
				int_error(c_token, "widget: expecting string");
			if (*s)
				qt_option->Widget = QString(s);
			free(s);
			break;
		case QT_FONT:
			SETCHECKDUP(set_font);
			if (!(s = try_to_get_string()))
				int_error(c_token, "font: expecting string");
			if (*s)
			{
				fontSettings = QString(s);
				QStringList list = fontSettings.split(',');
				if ((list.size() > 0) && !list[0].isEmpty())
					qt_option->FontName = list[0];
				if ((list.size() > 1) && (list[1].toInt() > 0))
					qt_optionFontSize = list[1].toInt();
			}
			free(s);
			break;
		case QT_ENHANCED:
			SETCHECKDUP(set_enhanced);
			qt_optionEnhanced = true;
			term->flags |= TERM_ENHANCED_TEXT;
			break;
		case QT_NOENHANCED:
			SETCHECKDUP(set_enhanced);
			qt_optionEnhanced = false;
			term->flags &= ~TERM_ENHANCED_TEXT;
			break;
		case QT_SIZE:
			SETCHECKDUP(set_size);
			if (END_OF_COMMAND)
				int_error(c_token, "size requires 'width,heigth'");
			qt_optionWidth = real_expression();
			if (!equals(c_token++, ","))
				int_error(c_token, "size requires 'width,heigth'");
			qt_optionHeight = real_expression();
			if (qt_optionWidth < 1 || qt_optionHeight < 1)
				int_error(c_token, "size is out of range");
			break;
		case QT_PERSIST:
			SETCHECKDUP(set_persist);
			qt_optionPersist = true;
			break;
		case QT_NOPERSIST:
			SETCHECKDUP(set_persist);
			qt_optionPersist = false;
			break;
		case QT_RAISE:
			SETCHECKDUP(set_raise);
			qt_optionRaise = true;
			break;
		case QT_NORAISE:
			SETCHECKDUP(set_raise);
			qt_optionRaise = false;
			break;
		case QT_CTRL:
			SETCHECKDUP(set_ctrl);
			qt_optionCtrl = true;
			break;
		case QT_NOCTRL:
			SETCHECKDUP(set_ctrl);
			qt_optionCtrl = false;
			break;
		case QT_TITLE:
			SETCHECKDUP(set_title);
			if (!(s = try_to_get_string()))
				int_error(c_token, "title: expecting string");
			if (*s)
				qt_option->Title = qt_encodingToCodec(encoding)->toUnicode(s);
			free(s);
			break;
		case QT_CLOSE:
			SETCHECKDUP(set_close);
			break;
		case QT_DASH:
			SETCHECKDUP(set_dash);
			qt_optionDash = true;
			break;
		case QT_DASHLENGTH:
			/* FIXME: This should be easy, at least in Qt 4.8 */
			c_token++;
			(void) real_expression();
			break;
		case QT_SOLID:
			SETCHECKDUP(set_dash);
			qt_optionDash = false;
			break;
		case QT_OTHER:
		default:
			qt_optionWindowId = int_expression();
			qt_option->Widget = "";
			if (set_number) duplication = true;
			set_number = true;
			break;
		}

		if (duplication)
			int_error(c_token-1, "Duplicated or contradicting arguments in qt term options.");
	}

	// Save options back into options string in normalized format
	QString termOptions = QString::number(qt_optionWindowId);

	/* Initialize user-visible font setting */
	fontSettings = qt_option->FontName + "," + QString::number(qt_optionFontSize);

	if (set_title)
	{
		termOptions += " title \"" + qt_option->Title + '"';
		if (qt)
			qt->out << GETitle << qt_option->Title;
	}

	if (set_size)
	{
		termOptions += " size " + QString::number(qt_optionWidth) + ", "
		                        + QString::number(qt_optionHeight);
		qt_setSize   = true;
		qt_setWidth  = qt_optionWidth;
		qt_setHeight = qt_optionHeight;
	}

	if (set_enhanced) termOptions += qt_optionEnhanced ? " enhanced" : " noenhanced";
	                  termOptions += " font \"" + fontSettings + '"';
	if (set_dash)     termOptions += qt_optionDash ? " dashed" : " solid";
	if (set_widget)   termOptions += " widget \"" + qt_option->Widget + '"';
	if (set_persist)  termOptions += qt_optionPersist ? " persist" : " nopersist";
	if (set_raise)    termOptions += qt_optionRaise ? " raise" : " noraise";
	if (set_ctrl)     termOptions += qt_optionCtrl ? " ctrl" : " noctrl";

	if (set_close && qt) qt->out << GECloseWindow << qt_optionWindowId;

	/// @bug change Utf8 to local encoding
	strncpy(term_options, termOptions.toUtf8().data(), MAX_LINE_LEN);
}

void qt_layer( t_termlayer syncpoint )
{
    static int current_plotno = 0;

    /* We must ignore all syncpoints that we don't recognize */
    switch (syncpoint) {
	case TERM_LAYER_BEFORE_PLOT:
		current_plotno++;
		qt->out << GEPlotNumber << current_plotno; break;
	case TERM_LAYER_AFTER_PLOT:
		qt->out << GEPlotNumber << 0; break;
	case TERM_LAYER_RESET:
	case TERM_LAYER_RESET_PLOTNO:
		if (!multiplot) current_plotno = 0; break;
	case TERM_LAYER_BEGIN_KEYSAMPLE:
		qt->out << GELayer << QTLAYER_BEGIN_KEYSAMPLE; break;
	case TERM_LAYER_END_KEYSAMPLE:
		qt->out << GELayer << QTLAYER_END_KEYSAMPLE; break;
	case TERM_LAYER_BEFORE_ZOOM:
		qt->out << GELayer << QTLAYER_BEFORE_ZOOM; break;
    	default:
		break;
    }
}

void qt_hypertext( int type, const char *text )
{
	if (type == TERM_HYPERTEXT_TOOLTIP)
		qt->out << GEHypertext << qt->codec->toUnicode(text);
}

#ifdef EAM_BOXED_TEXT
void qt_boxed_text(unsigned int x, unsigned int y, int option)
{
	qt->out << GETextBox << qt_termCoordF(x, y) << option;
}
#endif

void qt_modify_plots(unsigned int ops)
{
	if ((ops & MODPLOTS_INVERT_VISIBILITIES) == MODPLOTS_INVERT_VISIBILITIES) {
		qt->out << GEModPlots << QTMODPLOTS_INVERT_VISIBILITIES;
	} else if (ops & MODPLOTS_SET_VISIBLE) {
		qt->out << GEModPlots << QTMODPLOTS_SET_VISIBLE;
	} else if (ops & MODPLOTS_SET_INVISIBLE) {
		qt->out << GEModPlots << QTMODPLOTS_SET_INVISIBLE;
	}
	qt_flushOutBuffer();
}
