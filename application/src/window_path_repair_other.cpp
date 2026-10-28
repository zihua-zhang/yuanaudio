#include "window.h"
#include <QMessageBox>
void Window::showNasPathRepair() {
    QMessageBox::information(this, "批量处理路径问题", "此路径兼容功能当前适用于 Apple 芯片 Mac 与绿联 UGOS Pro NAS。");
}
