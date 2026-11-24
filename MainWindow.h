#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>

#include "SYW.h"

class QPlainTextEdit;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(SYW& syw, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildMenu();
    void wireSyw();
    void watchFile();
    void showLog();

    SYW& syw;
    QPlainTextEdit* text;
    QProcess editorProc;
    QFileSystemWatcher watcher;
    QTimer debounce;
};

#endif // MAINWINDOW_H