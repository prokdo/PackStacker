#include "gui/main_window.hpp"

#include "ui_main_window.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , _ui(std::make_unique<Ui::MainWindow>())
{
    _ui->setupUi(this);
    _initProgressBar();
    _initStorages();
    _initSignals();
}

MainWindow::~MainWindow() = default;

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

void MainWindow::_initStorages()
{
    _availableStorage = new PackStorage(this);
    _selectedStorage = new PackStorage(this);

    _ui->availableListView->setModel(_availableStorage);
    _ui->selectedListView->setModel(_selectedStorage);
}

void MainWindow::_initSignals() {}
