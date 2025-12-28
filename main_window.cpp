#include "main_window.hpp"
#include "ui_main_window.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui_(std::make_unique<Ui::MainWindow>())
{
    ui_->setupUi(this);
    initProgressBar_();
    initStorages_();
    initSignals_();
}

MainWindow::~MainWindow() {}

void MainWindow::initProgressBar_()
{
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->setFixedWidth(200);
    progressBar_->setStyleSheet("QProgressBar { margin-right: 9px; }");
    progressBar_->setVisible(false);

    ui_->statusBar->addPermanentWidget(progressBar_);
}

void MainWindow::initStorages_()
{
    availableStorage_ = new PackStorage(this);
    selectedStorage_ = new PackStorage(this);

    ui_->availableListView->setModel(availableStorage_);
    ui_->selectedListView->setModel(selectedStorage_);
}

void MainWindow::initSignals_() const
{

}
