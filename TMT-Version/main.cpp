// qt_tmt_terminal.cpp — Qt Terminal Widget using libtmt-revival (https://github.com/MurphyMc/libtmt-revival)

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QKeyEvent>
#include <QTimer>
#include <QFontMetrics>
#include <QVector>
#include <QColor>
#include <QResizeEvent>

extern "C" {
#include "tmt.h"
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
}

constexpr int TERM_ROWS = 24;
constexpr int TERM_COLS = 80;

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    TerminalWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        initFont();
        initPTY();
        initTMT();
        startTimer();
    }

    ~TerminalWidget() {
        if (vt) tmt_close(vt);
        if (pid > 0) kill(pid, SIGKILL);
        if (masterFd >= 0) ::close(masterFd);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        const TMTSCREEN *s = tmt_screen(vt);

        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                const TMTCHAR *ch = &s->lines[y]->chars[x];
                QChar qch = QString::fromUtf8(ch->c, ch->cw).at(0);
                QColor fg = ch->fg == TMT_COLOR_DEFAULT ? Qt::white : QColor::fromRgb(ch->fg);
                p.setPen(fg);
                p.drawText(x * charW, (y + 1) * charH - baseline, qch);
            }
        }

        if (s->cursor->visible) {
            p.fillRect(s->cursor->c * charW, s->cursor->r * charH, charW, charH, Qt::gray);
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        QByteArray bytes = e->text().toUtf8();
        if (e->key() == Qt::Key_Backspace) bytes = "\x7f";
        else if (e->key() == Qt::Key_Return) bytes = "\r";
        else if (e->key() == Qt::Key_Left) bytes = "\x1b[D";
        else if (e->key() == Qt::Key_Right) bytes = "\x1b[C";
        else if (e->key() == Qt::Key_Up) bytes = "\x1b[A";
        else if (e->key() == Qt::Key_Down) bytes = "\x1b[B";
        if (!bytes.isEmpty()) write(masterFd, bytes.data(), bytes.size());
    }

    void resizeEvent(QResizeEvent *) override {
        cols = width() / charW;
        rows = height() / charH;
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(masterFd, TIOCSWINSZ, &ws);
        kill(pid, SIGWINCH);
    }

private:
    TMT *vt = nullptr;
    int masterFd = -1;
    pid_t pid = -1;
    int rows = TERM_ROWS, cols = TERM_COLS;
    int charW = 10, charH = 18, baseline = 4;

    void initFont() {
        QFont f("Courier", 12);
        setFont(f);
        QFontMetrics fm(f);
        charW = fm.horizontalAdvance('M');
        charH = fm.height();
        baseline = fm.descent();
    }

    void initPTY() {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        pid = forkpty(&masterFd, nullptr, nullptr, &ws);
        if (pid == 0) {
            setenv("TERM", "xterm-256color", 1);
            execlp("bash", "bash", nullptr);
            perror("exec failed");
            _exit(1);
        }
        fcntl(masterFd, F_SETFL, O_NONBLOCK);
    }

    static void tmtCallback(tmt_msg_t m, TMT *vt, const void *, void *u) {
        if (m == TMT_MSG_SCREEN) static_cast<TerminalWidget*>(u)->update();
    }

    void initTMT() {
        vt = tmt_open(rows, cols, tmtCallback, this, nullptr);
    }

    void startTimer() {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &TerminalWidget::readPTY);
        timer->start(10);
    }

    void readPTY() {
        char buf[4096];
        int n = read(masterFd, buf, sizeof(buf));
        if (n > 0) tmt_write(vt, buf, n);
    }
};

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    TerminalWidget w;
    w.setWindowTitle("libtmt-revival Qt Terminal");
    w.resize(800, 450);
    w.show();
    return a.exec();
}

#include "qt_tmt_terminal.moc"
