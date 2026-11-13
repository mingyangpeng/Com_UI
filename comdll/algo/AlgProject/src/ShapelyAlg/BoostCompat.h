#ifndef SHAPELY_BOOST_COMPAT_H
#define SHAPELY_BOOST_COMPAT_H

// Boost 1.68 + MSVC 2022 兼容垫片（踩坑记录，详见 memory/project-shapely-geos.md）。
//
// 问题：pcl/io/boost.h → boost/iostreams/device/mapped_file.hpp → positioning.hpp
//   的 streampos_to_fpos_t() 调 std::fpos::seekpos()，该非标成员已被新 MSVC STL
//   移除（STL4019），编译报 C2039 "seekpos 不是 std::fpos 的成员"。
//
// 用法：任何 .cpp 在 include <pcl/io/*.h> 之前，必须先 include 本头：
//   #include "BoostCompat.h"
//   #include <pcl/io/pcd_io.h>
//
// 原理：把 positioning.hpp 的两个守卫宏预置为"已包含"（跳过坏实现），
// 并提供修正版 position_to_offset / offset_to_position（streambuf 只用这两个；
// _Fpos 恒为 0，streamoff 即完整位置，与旧 STL 语义等价）。

#include <istream>   // 完整 fpos / streampos 定义

#define BOOST_IOSTREAMS_DETAIL_CONFIG_FPOS_HPP_INCLUDED
#define BOOST_IOSTREAMS_POSITIONING_HPP_INCLUDED

#include <boost/cstdint.hpp>

namespace boost { namespace iostreams {

typedef boost::intmax_t stream_offset;

inline std::streamoff stream_offset_to_streamoff(stream_offset off) { return off; }
inline std::streampos offset_to_position(stream_offset off) { return off; }

// 修正版：不再调用已删除的 fpos::seekpos()/get_fpos_t()
inline stream_offset position_to_offset(std::streampos pos)
{
    return static_cast<stream_offset>(static_cast<std::streamoff>(pos));
}

} } // namespace boost::iostreams

#endif // SHAPELY_BOOST_COMPAT_H
