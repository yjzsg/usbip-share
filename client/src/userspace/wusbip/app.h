/*
 * Copyright (c) 2023-2025 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <wx/app.h>

class App : public wxApp
{
public:
        App();

        bool OnInit() override;
        void write_appearance(_In_ int val);

private:
        static inline auto &m_appearance = L"Appearance";

        void set_names();
        Appearance set_appearance();
        void on_end_session(_In_ wxCloseEvent &event);

        /*
         * 单实例保护: 见 app.cpp 的实现说明。
         * m_instance_mutex 在整个进程生命周期内持有,析构(进程退出)时自动释放。
         */
        void *m_instance_mutex{};
        bool acquire_single_instance();
        void activate_existing_instance();

public:
        /*
         * 第二个实例启动时用来"叫醒"已运行实例的自定义消息。
         * 用 RegisterWindowMessageW 注册,保证全系统唯一且不会被别的程序误收。
         */
        static unsigned int instance_message();
};


wxDECLARE_APP(App);
