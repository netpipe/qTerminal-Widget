// qt_terminal_widget.cpp (with color, cursor control, mouse support, fixed struct assignment)

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QFontDatabase>
#include <QScrollBar>
#include <QRegularExpression>

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

constexpr int TERM_ROWS = 24;
constexpr int TERM_COLS = 80;

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    TerminalWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        initFont();
        startPTY();
        startTimer();
    }

    ~TerminalWidget() {
        if (pid > 0)
            kill(pid, SIGKILL);
        if (masterFd >= 0)
            ::close(masterFd);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);

        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const Cell &cell = screen[y][x];
                if (cell.ch.isNull()) continue;
                p.setPen(cell.color);
                p.drawText(x * charWidth, (y + 1) * charHeight - baseline, cell.ch);
            }
        }

        if (cursorVisible) {
            p.setPen(Qt::white);
            p.fillRect(QRect(cursorX * charWidth, cursorY * charHeight, charWidth, charHeight), Qt::white);
            if (cursorY < rows && cursorX < cols) {
                p.setPen(Qt::black);
                p.drawText(cursorX * charWidth, (cursorY + 1) * charHeight - baseline, screen[cursorY][cursorX].ch);
            }
        }
    }


    void keyPressEvent(QKeyEvent *event) override {
        QByteArray input;

        if (event->modifiers() & Qt::ControlModifier && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
            input.append(char(event->key() - Qt::Key_A + 1));  // Ctrl+A → \x01
        } else {
            switch(event->key()) {
                case Qt::Key_Backspace: input = "\x7f"; break;
                case Qt::Key_Delete:    input = "\x1B[3~"; break;
                case Qt::Key_Return:
                case Qt::Key_Enter:     input = "\r"; break;
                case Qt::Key_Tab:       input = "\t"; break;
                case Qt::Key_Escape:    input = "\x1B"; break;
                case Qt::Key_Left:      input = "\x1B[D"; break;
                case Qt::Key_Right:     input = "\x1B[C"; break;
                case Qt::Key_Up:        input = "\x1B[A"; break;
                case Qt::Key_Down:      input = "\x1B[B"; break;
                default:
                    input = event->text().toUtf8();
                    break;
            }
        }

        if (!input.isEmpty() && masterFd >= 0)
            write(masterFd, input.constData(), input.length());
    }




    void mousePressEvent(QMouseEvent *event) override {
        int x = event->x() / charWidth;
        int y = event->y() / charHeight;
        QByteArray seq;
        seq.append("\x1B[M");
        seq.append(32 + 0); // left button
        seq.append(32 + x);
        seq.append(32 + y);
        if (masterFd >= 0)
            write(masterFd, seq.constData(), seq.length());
    }

    void resizeEvent(QResizeEvent *) override {
        cols = width() / charWidth;
        rows = height() / charHeight;
        screen.resize(rows);
        for (auto &row : screen)
            row.resize(cols);

        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(masterFd, TIOCSWINSZ, &ws);
        kill(pid, SIGWINCH);
    }

private:
    struct Cell {
        QChar ch;
        QColor color;
        Cell() : ch(' '), color(Qt::white) {}
        Cell(QChar c, QColor col) : ch(c), color(col) {}
    };

    int masterFd = -1;
    pid_t pid = -1;
    QVector<QVector<Cell>> screen = QVector<QVector<Cell>>(TERM_ROWS, QVector<Cell>(TERM_COLS));
    int rows = TERM_ROWS;
    int cols = TERM_COLS;
    int cursorX = 0, cursorY = 0;
    int charWidth = 10, charHeight = 18, baseline = 4;
    QColor currentColor = Qt::white;
    bool cursorVisible = true;
    QTimer *cursorTimer;

    void initFont() {
        QFont f("Courier", 12);
        setFont(f);
        QFontMetrics fm(f);
        charWidth = fm.horizontalAdvance('M');
    //    charHeight = fm.height();
        baseline = fm.ascent();
        charHeight = fm.height() + 2; // slight padding

    }

    void startPTY() {
        struct winsize ws = { TERM_ROWS, TERM_COLS, 0, 0 };
        pid = forkpty(&masterFd, nullptr, nullptr, &ws);
        if (pid == 0) {
            setenv("TERM", "xterm-256color", 1);
           // execlp("nano", "nano", nullptr);
            execlp("bash", "bash", nullptr);
            perror("exec failed");
            _exit(1);
        }
        fcntl(masterFd, F_SETFL, O_NONBLOCK);
    }

    void startTimer() {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &TerminalWidget::readFromPty);
        timer->start(10);
        cursorTimer = new QTimer(this);
        connect(cursorTimer, &QTimer::timeout, this, [this]() {
            cursorVisible = !cursorVisible;
            update();
        });
        cursorTimer->start(500); // blink every 500ms

    }

    void readFromPty() {
        if (masterFd < 0) return;
        char buf[4096];
        int n = read(masterFd, buf, sizeof(buf));
        if (n > 0)
            handleOutput(QByteArray::fromRawData(buf, n));
    }

    void handleOutput(const QByteArray &data) {
        static QByteArray escBuf;
        int i = 0;
        while (i < data.size()) {
            uchar byte = data[i];
            if (!escBuf.isEmpty() || byte == '\x1B') {
                escBuf.append(byte);
                if (byte >= '@' && byte <= '~') {
                    parseEscapeSequence(escBuf);
                    escBuf.clear();
                }
                ++i;
                continue;
            }

            if (byte == '\n') {
                cursorX = 0;
                cursorY = qMin(cursorY + 1, rows - 1);
            } else {
                if (cursorY < rows && cursorX < cols)
                    screen[cursorY][cursorX] = Cell(QChar(byte), currentColor);
                cursorX++;
                if (cursorX >= cols) {
                    cursorX = 0;
                    cursorY = qMin(cursorY + 1, rows - 1);
                }
            }
            ++i;
        }
        update();
    }

    void parseEscapeSequence(const QByteArray &seq) {
        if (seq.startsWith("\x1B[")) {
            QRegularExpression regex("\\x1B\\[(\d+)(;(\d+))*m");
            QRegularExpressionMatch match = regex.match(seq);
            if (match.hasMatch()) {
                int code = match.captured(1).toInt();
                switch (code) {
                    case 30: currentColor = Qt::black; break;
                    case 31: currentColor = Qt::red; break;
                    case 32: currentColor = Qt::green; break;
                    case 33: currentColor = Qt::yellow; break;
                    case 34: currentColor = Qt::blue; break;
                    case 35: currentColor = Qt::magenta; break;
                    case 36: currentColor = Qt::cyan; break;
                    case 37: currentColor = Qt::white; break;
                    default: break;
                }
            }
        }
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TerminalWidget term;
    term.setWindowTitle("Qt Terminal Grid");
    term.resize(TERM_COLS * 10, TERM_ROWS * 18);
    term.show();
    return app.exec();
}

#include "main.moc"
