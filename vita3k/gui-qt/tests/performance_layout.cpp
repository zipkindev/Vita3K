// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ui_settings_dialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QScrollArea>
#include <array>
#include <cassert>
#include <gui-qt/settings_tabs.h>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QDialog dialog;
    Ui::vita3k_settings_dialog ui;
    ui.setupUi(&dialog);
    const std::array<QWidget *, 11> pages{ ui.core_tab, ui.cpu_tab, ui.gpu_tab, ui.performance_tab,
        ui.audio_tab, ui.camera_tab, ui.system_tab, ui.emulator_tab, ui.gui_tab, ui.network_tab, ui.debug_tab };
    assert(ui.tab_widget_settings->count() == pages.size());
    assert(ui.tab_widget_settings->indexOf(ui.performance_tab) == static_cast<int>(SettingsTab::Performance));
    assert(ui.tab_widget_settings->indexOf(ui.audio_tab) == static_cast<int>(SettingsTab::Audio));
    assert(ui.tab_widget_settings->indexOf(ui.debug_tab) == static_cast<int>(SettingsTab::Debug));
    for (size_t i = 0; i < pages.size(); ++i)
        assert(ui.tab_widget_settings->widget(static_cast<int>(i)) == pages[i]);
    ui.tab_widget_settings->setCurrentWidget(ui.performance_tab);
    const std::array<QCheckBox *, 5> switches{ ui.automatic_texture_preparation, ui.parallel_texture_decode,
        ui.jit_compile_telemetry, ui.sync_wait_telemetry, ui.decoded_texture_telemetry };
    for (const auto size : { QSize(780, 676), QSize(1280, 720) }) {
        dialog.resize(size);
        dialog.show();
        app.processEvents();
        for (auto *control : switches) {
            assert(ui.performance_tab->isAncestorOf(control));
            assert(!ui.gb_fps_hack->isAncestorOf(control));
            assert(control->width() >= control->minimumSizeHint().width());
            assert(control->height() >= control->minimumSizeHint().height());
            ui.performance_scroll->ensureWidgetVisible(control);
            app.processEvents();
            const auto top = control->mapTo(ui.performance_scroll->viewport(), QPoint(0, 0));
            assert(top.x() >= 0 && top.y() >= 0);
            assert(top.x() + control->width() <= ui.performance_scroll->viewport()->width());
            assert(top.y() + control->height() <= ui.performance_scroll->viewport()->height());
            const QRect box(control->mapTo(&dialog, QPoint(0, 0)), control->size());
            for (auto *other : switches) {
                if (other == control)
                    continue;
                assert(!box.intersects(QRect(other->mapTo(&dialog, QPoint(0, 0)), other->size())));
            }
        }
    }
}
