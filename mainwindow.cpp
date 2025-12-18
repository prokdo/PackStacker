#include "mainwindow.hpp"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , _ui(new Ui::MainWindow)
{
    _ui->setupUi(this);
    _initProgressBar();
}

MainWindow::~MainWindow()
{
    delete _ui;
}

void MainWindow::_initProgressBar()
{
    _progressBar = new QProgressBar(this);
    _progressBar->setRange(0, 100);
    _progressBar->setValue(0);
    _progressBar->setTextVisible(false);
    _progressBar->setFixedWidth(200);
    _progressBar->setStyleSheet("QProgressBar { margin-right: 9px; }");
    _progressBar->setVisible(false);

    _ui->statusBar->addPermanentWidget(_progressBar);
}
