#pragma once

#include <QMainWindow>
#include <QProgressBar>

#include "storage/pack_storage.hpp"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

    public:
        explicit MainWindow(QWidget* parent = nullptr);
        ~MainWindow();

    private:
        std::unique_ptr<Ui::MainWindow> ui_;

        QProgressBar* progressBar_;
        void initProgressBar_();

        PackStorage* availableStorage_;
        PackStorage* selectedStorage_;
        void initStorages_();

        void initSignals_() const;
};
