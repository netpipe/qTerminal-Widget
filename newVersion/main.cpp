// qt_terminal_libvterm.cpp
// Qt Terminal Widget using libvterm + forkpty for proper VT emulation with colors, cursor, keyboard.

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QKeyEvent>
#include <QFontMetrics>
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <vterm.h>

constexpr int TERM_ROWS = 24;
constexpr int TERM_COLS = 80;

struct Cell {
    QChar ch;
    QColor fg;
    QColor bg;
    bool bold;
    bool underline;
    bool inverse;

    Cell() : ch(' '), fg(Qt::white), bg(Qt::black),
             bold(false), underline(false), inverse(false) {}
};

typedef struct {
    VTermColor fg;  // foreground color
    VTermColor bg;  // background color

} VTermScreenCellAttrs;


class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    TerminalWidget(QWidget* parent = nullptr)
        : QWidget(parent),
          vterm(nullptr),
          screen(nullptr),
          masterFd(-1),
          pid(-1),
          cursorVisible(true),
          blinkState(false),
          charWidth(10),
          charHeight(18),
          baseline(4)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        initFont();
        initVTerm();
        startPTY();
        startTimers();

        // Initialize screen buffer with blank cells
        screenBuffer.resize(TERM_ROWS);
        for (auto &row : screenBuffer)
            row.resize(TERM_COLS);
    }

    ~TerminalWidget() override {
        if (pid > 0)
            kill(pid, SIGKILL);
        if (masterFd >= 0)
            ::close(masterFd);
        if (vterm) {
            vterm_free(vterm);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);

        for (int y = 0; y < TERM_ROWS; ++y) {
            for (int x = 0; x < TERM_COLS; ++x) {
                const Cell &c = screenBuffer[y][x];

                QColor fg = c.fg;
                QColor bg = c.bg;

                if (c.inverse) std::swap(fg, bg);

                // Paint background rect
                p.fillRect(x * charWidth, y * charHeight, charWidth, charHeight, bg);

                // Paint text
                p.setPen(fg);
                QFont font = this->font();
                font.setBold(c.bold);
                font.setUnderline(c.underline);
                p.setFont(font);

                if (!c.ch.isNull() && c.ch != QChar(' '))
                    p.drawText(x * charWidth, (y + 1) * charHeight - baseline, c.ch);
            }
        }

        // Draw blinking cursor
        if (cursorVisible && blinkState) {
            p.fillRect(cursorX * charWidth, cursorY * charHeight, charWidth, charHeight, Qt::white);

            if (cursorY < TERM_ROWS && cursorX < TERM_COLS) {
                p.setPen(Qt::black);
                QChar c = screenBuffer[cursorY][cursorX].ch;
                if (!c.isNull() && c != QChar(' '))
                    p.drawText(cursorX * charWidth, (cursorY + 1) * charHeight - baseline, c);
            }
        }
    }

    void keyPressEvent(QKeyEvent *event) override {
        QByteArray input;

        // Map special keys to VT sequences
        switch (event->key()) {
        case Qt::Key_Backspace:
            input = "\x7f";
            break;
        case Qt::Key_Return:
            input = "\r";
            break;
        case Qt::Key_Left:
            input = "\x1b[D";
            break;
        case Qt::Key_Right:
            input = "\x1b[C";
            break;
        case Qt::Key_Up:
            input = "\x1b[A";
            break;
        case Qt::Key_Down:
            input = "\x1b[B";
            break;
        case Qt::Key_Delete:
            input = "\x1b[3~";
            break;
        case Qt::Key_Home:
            input = "\x1b[H";
            break;
        case Qt::Key_End:
            input = "\x1b[F";
            break;
        case Qt::Key_PageUp:
            input = "\x1b[5~";
            break;
        case Qt::Key_PageDown:
            input = "\x1b[6~";
            break;
        default:
            if (event->modifiers() & Qt::ControlModifier) {
                char c = event->key();
                if (c >= '@' && c <= '_') input.append(c - '@');
            } else {
                input = event->text().toUtf8();
            }
            break;
        }

        if (!input.isEmpty() && masterFd >= 0) {
            ::write(masterFd, input.constData(), input.size());
        }
    }

    void resizeEvent(QResizeEvent *) override {
        int newCols = width() / charWidth;
        int newRows = height() / charHeight;

        if (newCols != TERM_COLS || newRows != TERM_ROWS) {
            // We do not resize libvterm dynamically here, but you could recreate vterm.
            // Just update internal vars and repaint for now:
            // (optional: implement dynamic resize later)
        }
    }

private slots:
    void onReadPTY() {
        if (masterFd < 0)
            return;

        char buf[4096];
        ssize_t n = read(masterFd, buf, sizeof(buf));
        if (n > 0) {
            vterm_input_write(vterm, buf, n);
            updateScreenFromVTerm();
            update();
        }
    }

    void onCursorBlink() {
        blinkState = !blinkState;
        update();
    }

private:
    VTerm *vterm;
    VTermScreen *screen;
    int masterFd;
    pid_t pid;

    int cursorX = 0, cursorY = 0;
    bool cursorVisible;
    bool blinkState;

    int charWidth, charHeight, baseline;

    QVector<QVector<Cell>> screenBuffer;

    void initFont() {
        QFont f("Courier New", 12);
        setFont(f);
        QFontMetrics fm(f);
        charWidth = fm.horizontalAdvance('M');
        charHeight = fm.height();
        baseline = fm.descent();
    }

    static int vtermScreenDamage(VTermRect rect, void *user) {
        // Can be used to mark region dirty, but we'll just update whole widget
        Q_UNUSED(rect);
        TerminalWidget *term = static_cast<TerminalWidget*>(user);
        term->update();
        return 0;
    }

    void initVTerm() {
        vterm = vterm_new(TERM_ROWS, TERM_COLS);
        vterm_set_utf8(vterm, 1);
        screen = vterm_obtain_screen(vterm);
        vterm_screen_reset(screen, 1);

        VTermScreenCallbacks cb {};
        cb.damage = &TerminalWidget::vtermScreenDamage;
        vterm_screen_set_callbacks(screen, &cb, this);
    }

    void startPTY() {
        struct winsize ws { TERM_ROWS, TERM_COLS, 0, 0 };

        pid = forkpty(&masterFd, nullptr, nullptr, &ws);
        if (pid == 0) {
            setenv("TERM", "xterm-256color", 1);
            execlp("bash", "bash", nullptr);
            perror("exec failed");
            _exit(1);
        }
        fcntl(masterFd, F_SETFL, O_NONBLOCK);
    }

    void startTimers() {
        QTimer *readTimer = new QTimer(this);
        connect(readTimer, &QTimer::timeout, this, &TerminalWidget::onReadPTY);
        readTimer->start(10);

        QTimer *blinkTimer = new QTimer(this);
        connect(blinkTimer, &QTimer::timeout, this, &TerminalWidget::onCursorBlink);
        blinkTimer->start(500);
    }

    void updateScreenFromVTerm() {
        for (int row = 0; row < TERM_ROWS; ++row) {
            for (int col = 0; col < TERM_COLS; ++col) {
                VTermScreenCell cell;
                VTermPos pos = { row, col };
                vterm_screen_get_cell(screen, pos, &cell);



                Cell &c = screenBuffer[row][col];

                // Handle UTF-8 char (only first char, ignoring wide chars)
                if (cell.chars[0])
                    c.ch = QChar(cell.chars[0]);
                else
                    c.ch = QChar(' ');

                // Translate attributes to colors & styles
                c.bold = (cell.attrs.bold != 0);
                c.underline = (cell.attrs.underline != 0);
                c.inverse = (cell.attrs.reverse != 0);

                c.fg = qtColorFromVTermColor(cell.attrs.fg);
                c.bg = qtColorFromVTermColor(cell.attrs.bg);
            }
        }

        VTermPos pos;
        vterm_screen_get_cursor_pos(screen, &pos);
        cursorY = pos.row;
        cursorX = pos.col;
    }

    QColor qtColorFromVTermColor(VTermColor c) {
        if (c.type == VTERM_COLOR_RGB) {
            return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
        } else if (c.type == VTERM_COLOR_DEFAULT) {
            return Qt::white;
        } else if (c.type == VTERM_COLOR_INDEX) {
            static const QColor colors[16] = {
                Qt::black, Qt::red, Qt::green, Qt::yellow,
                Qt::blue, Qt::magenta, Qt::cyan, Qt::white,
                QColor(128,128,128), QColor(255,0,0), QColor(0,255,0), QColor(255,255,0),
                QColor(0,0,255), QColor(255,0,255), QColor(0,255,255), QColor(255,255,255)
            };
            int idx = c.index;
            if (idx < 0 || idx > 15)
                idx = 7;
            return colors[idx];
        }
        return Qt::white;
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    TerminalWidget term;
    term.resize(800, 450);
    term.show();

    return app.exec();
}

#include "main.moc"
