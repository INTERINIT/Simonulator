<div align="center">
 
# Simonulator<br />
基于MAME的IBM Simon模拟器<br />

[![LICENSE](https://img.shields.io/badge/LICENSE-GPL2.0-blue.svg?style=for-the-badge)](https://github.com/Inter1006/PenPointOS_Vbox/blob/main/LICENSE )

Language  语言<br />
简体中文  |  [ENGLISH](https://github.com/INTERINIT/Simonulator/blob/main/README.md)<br />


</div>

## 📝Simonulator是什么?
Simonulator 是一款基于 MAME 的模拟器，旨在模拟 IBM Simon 并尽可能复刻其功能。<br />
有关IBM Simon的信息，请参阅[维基百科](https://en.wikipedia.org/wiki/IBM_Simon).

## 📚当前进展
截止到2026/10/03,本项目取得了如下进展<br />
<br />
已实现:<br />

* VG230两个ROM的正确挂载
* 正确的宽高比和显示方向
* 模拟触摸屏输入
* 背光系统
* RTC时钟
* 设备状态指示灯
* PCMCIA卡支持
* 蜂鸣音
* 机身物理按键映射
* 待机模式

初步实现:<br />

* 电话功能 (目前可以在虚拟机间接打电话).
* 机身底部的串口
* 邮件和传真

尚未实现:<br />
* 紧急通话(会报错)
* ~请尽管在issue里许愿~


## 📥如何使用?
**下载** <br />
你可以在[此处](https://github.com/INTERINIT/Simonulator/releases)找到预构建版本.<br />

**开始前的准备工作** <br />
在开始之前，你需要一份Simon的完整转储. <br />
该转储应有以下内容: <br />
1.Simon的1MB FlashROM，命名为"SIMONFlash.bin" <br />
2.Simon的128KB BIOS chip, 命名为"simonbios.bin" <br />
(上述文件应放在`\roms\ibmsimon`里)<br />

*由于种种原因，本项目不提供任何转储副本，不过你可以关注[这个项目](https://github.com/INTERINIT/SimonDump).*

**关于物理按键**<br />
Simonulator 现在支持物理按键模拟<br />
|电脑按键|Simon操作|
|-------------|------------------------|
|F9|旋转屏幕|
|F10|拨动开关(进入待机模式)|
|Page Up/Down|侧边上下键|

**使用虚拟交换机** <br />
运行 `\Start_Switch.bat` 以启动交换机<br />

~常用命令:~<br />
*2026/10/03后不再需要常用命令，虚拟交换机已具备GUI*

|命令|含义|举例|
|-------------|------------------------|-----------------|
|list         |列出所有在线设备 |                 |
|ring A B     |以B的身份呼叫A|ring 1001 1002   |
|hang A       |将A设为挂机状态(强制中断通话).|hang 1001 |
set A [Service Status] [signal strength] [Operator Name] |设置A的状态信息|set 1001 Home1 6 helloworld
|clear        |清空控制台|                 |
|quit         |退出交换机|                 |
<br />

**如何编译?** <br />
请看[编译指南](https://github.com/INTERINIT/Simonulator/blob/main/docs/BUILDING.md)

## 🛠️Under Construction
建设中/Under Construction

## 关于AI的使用
本项目的如下部分使用了**Codex**: <br />
1.Automatic capture and analysis of some logs <br />
2.Partial reverse engineering of C:\Phone.exe <br />
3.Cleanup of the working directory<br />

**其余所有代码均由人工编写，AI生成的所有代码已经过人工审查.**


## ❗已知问题
由于MAME的性能问题，电话硬件启动后，整体反应速度会下降，遇到吞点击的时候可以长按一会试试.<br />
我们建议您在使用其它功能时关闭电话电源.<br />

## ℹ关于
**作者:INTERINIT**
<table>
  <tr>
    <td align="center"><a href="https://github.com/INTERINIT"><img src="https://github.com/INTERINIT.png" width="100px;" alt=""/><br /><sub><b>Github Page</b></sub></a><br /></td>
    <td align="center"><a href="https://space.bilibili.com/1756824708"><img src="https://github.com/user-attachments/assets/48a9c033-8c87-4fa2-b5f9-1be5a9c9d665" width="100px;" alt=""/><br /><sub><b>bilibili</b></sub></a><br /></td>
  </tr>
</table>

**特别鸣谢(排名不分先后)**
<table>
  <tr>
    <td align="center"><a href="https://space.bilibili.com/484165196"><img src="https://github.com/WindowsNT351.png" width="100px;" alt=""/><br /><sub><b>351Workshop<br />bilibili</b></sub></a><br /></td>
    <td align="center"><a href="https://github.com/simoneer"><img src="https://github.com/simoneer.png" width="100px;" alt=""/><br /><sub><b>Simoneer<br />Github Page</b></sub></a><br /></td>
    <td align="center"><a href="https://space.bilibili.com/353176009"><img src="https://github.com/user-attachments/assets/079e9e4f-ce7a-4bce-8044-818f484f657e" width="100px;" alt=""/><br /><sub><b>Digitaalia<br />bilibili</b></sub></a><br /></td>
  </tr>
  
</table>
<br />

我们的QQ群:**981893945** <br />
欢迎来玩.

## 🤝友站链接
[351Workshop官方网站](https://www.351workshop.top/)<br />
[Simon History](https://simoneer.github.io/history/)<br />
