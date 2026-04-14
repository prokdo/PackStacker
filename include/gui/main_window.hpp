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

        static inline constexpr char APP_NAME[] = "PackStacker";

    private:
        std::unique_ptr<Ui::MainWindow> _ui;

        QProgressBar* _progressBar;
        void _initProgressBar();

        PackStorage* _availableStorage;
        PackStorage* _selectedStorage;
        void _initStorages();

        void _initSignals();
};
