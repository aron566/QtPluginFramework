/// @file plugin_serialization.cpp
/// @brief 插件 IPC 用 protobuf 转换实现(C++ 结构 ↔ proto 消息)
/// @details 字段覆盖与旧 QDataStream 算子一致(见 plugin_ipc.proto 注释);
///          枚举(TopoEventKind/GraphicsEventType)数值与 C++ 侧一致,直接
///          static_cast,边界值以 static_assert 钉死,漂移即编译失败。
#include "plugin_serialization.h"

#include <QBuffer>
#include <QImage>

namespace plugin_proto {
namespace {

// ---- 小工具 ----
std::string to_std(const QByteArray& b) {
    if (b.isEmpty()) return std::string();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}
QByteArray to_qba(const std::string& s) {
    if (s.empty()) return QByteArray();
    return QByteArray(s.data(), static_cast<int>(s.size()));
}
std::string to_std(const QVector<quint8>& v) {
    if (v.isEmpty()) return std::string();
    return std::string(reinterpret_cast<const char*>(v.constData()),
                       static_cast<size_t>(v.size()));
}
QVector<quint8> to_quint8_vec(const std::string& s) {
    QVector<quint8> v;
    v.reserve(static_cast<int>(s.size()));
    for (char c : s) v.append(static_cast<quint8>(c));
    return v;
}

} // namespace

// 枚举数值漂移即编译失败(.proto 注释已写明"与 C++ 侧数值一致")
static_assert(static_cast<int>(TopoEventKind::Other) == 11,
              "TopoEventKind order drifted vs plugin_ipc.proto");
static_assert(static_cast<int>(GraphicsEventType::MouseDblClick) == 6,
              "GraphicsEventType order drifted vs plugin_ipc.proto");

// ---- QImage ↔ PNG ----
QByteArray image_to_png(const QImage& img) {
    if (img.isNull()) return QByteArray();
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}
QImage image_from_png(const QByteArray& png) {
    if (png.isEmpty()) return QImage();
    return QImage::fromData(png, "PNG");
}

// ---- PhysicalMeta ----
qtplugin::ipc::PhysicalMeta to_proto(const PhysicalMeta& m) {
    qtplugin::ipc::PhysicalMeta p;
    p.set_timestamp(m.timestamp);
    p.set_phr_mcs(m.phr_mcs);
    p.set_option(m.option);
    p.set_channel(m.channel);
    p.set_is_rf(m.is_rf);
    return p;
}
PhysicalMeta from_proto(const qtplugin::ipc::PhysicalMeta& p) {
    PhysicalMeta m;
    m.timestamp = p.timestamp();
    m.phr_mcs = static_cast<quint8>(p.phr_mcs());
    m.option = static_cast<quint8>(p.option());
    m.channel = static_cast<quint16>(p.channel());
    m.is_rf = p.is_rf();
    return m;
}

// ---- BplcFrame ----
qtplugin::ipc::BplcFrame to_proto(const BplcFrame& f) {
    qtplugin::ipc::BplcFrame p;
    *p.mutable_meta() = to_proto(f.meta);
    p.set_data(to_std(f.data));
    p.set_error_reason(f.error_reason.toStdString());
    p.set_arrival_us(f.arrival_us);
    p.set_raw_wire(to_std(f.raw_wire));
    p.set_decoded_index(f.decoded_index);
    p.set_decoded_epoch_ms(f.decoded_epoch_ms);
    *p.mutable_topo_event() = to_proto(f.topo_event);
    p.set_accepted(f.accepted);
    *p.mutable_mpdu() = to_proto(f.mpdu);
    p.set_msdu_summary(f.msdu_summary.toStdString());
    p.set_msdu_present(f.msdu_present);
    return p;
}
BplcFrame from_proto(const qtplugin::ipc::BplcFrame& p) {
    BplcFrame f;
    f.meta = from_proto(p.meta());
    f.data = to_qba(p.data());
    f.error_reason = QString::fromStdString(p.error_reason());
    f.arrival_us = p.arrival_us();
    f.raw_wire = to_qba(p.raw_wire());
    f.decoded_index = p.decoded_index();
    f.decoded_epoch_ms = p.decoded_epoch_ms();
    f.topo_event = from_proto(p.topo_event());
    f.accepted = p.accepted();
    f.mpdu = from_proto(p.mpdu());
    f.msdu_summary = QString::fromStdString(p.msdu_summary());
    f.msdu_present = p.msdu_present();
    return f;
}

// ---- MsduState ----
qtplugin::ipc::MsduState to_proto(const MsduState& s) {
    qtplugin::ipc::MsduState p;
    p.set_buffer(to_std(s.buffer));
    p.set_received_count(s.received_count);
    p.set_expected_len(s.expected_len);
    p.set_complete(s.complete);
    return p;
}
MsduState from_proto(const qtplugin::ipc::MsduState& p) {
    MsduState s;
    s.buffer = to_qba(p.buffer());
    s.received_count = p.received_count();
    s.expected_len = p.expected_len();
    s.complete = p.complete();
    return s;
}

// ---- ParseFilter ----
qtplugin::ipc::ParseFilter to_proto(const ParseFilter& f) {
    qtplugin::ipc::ParseFilter p;
    p.set_enable_type_filter(f.enable_type_filter);
    p.set_allow_beacon(f.allow_beacon);
    p.set_allow_sof(f.allow_sof);
    p.set_allow_ack(f.allow_ack);
    p.set_allow_coord(f.allow_coord);
    p.set_link_hplc(f.link_hplc);
    p.set_link_hrf(f.link_hrf);
    p.set_nid_filter(f.nid_filter);
    p.set_nid_mask(f.nid_mask);
    for (quint32 n : f.nid_list) p.add_nid_list(n);
    p.set_tei_filter(f.tei_filter);
    for (quint16 t : f.tei_list) p.add_tei_list(t);
    return p;
}
ParseFilter from_proto(const qtplugin::ipc::ParseFilter& p) {
    ParseFilter f;
    f.enable_type_filter = p.enable_type_filter();
    f.allow_beacon = p.allow_beacon();
    f.allow_sof = p.allow_sof();
    f.allow_ack = p.allow_ack();
    f.allow_coord = p.allow_coord();
    f.link_hplc = p.link_hplc();
    f.link_hrf = p.link_hrf();
    f.nid_filter = p.nid_filter();
    f.nid_mask = p.nid_mask();
    for (int i = 0; i < p.nid_list_size(); ++i)
        f.nid_list.append(p.nid_list(i));
    f.tei_filter = p.tei_filter();
    for (int i = 0; i < p.tei_list_size(); ++i)
        f.tei_list.append(static_cast<quint16>(p.tei_list(i)));
    return f;
}

// ---- MpduInfo ----
qtplugin::ipc::MpduInfo to_proto(const MpduInfo& m) {
    qtplugin::ipc::MpduInfo p;
    p.set_frame_type(m.frame_type);
    p.set_net_type(m.net_type);
    p.set_net_id(m.net_id);
    p.set_version(m.version);
    p.set_src_tei(m.src_tei);
    p.set_dst_tei(m.dst_tei);
    p.set_link_id(m.link_id);
    p.set_frame_len(m.frame_len);
    p.set_pb_num(m.pb_num);
    p.set_symbol_num(m.symbol_num);
    p.set_bc_flag(m.bc_flag);
    p.set_re_send_flag(m.re_send_flag);
    p.set_encryp_flag(m.encryp_flag);
    p.set_tmi(m.tmi);
    p.set_tmi_ext(m.tmi_ext);
    p.set_pb_size(m.pb_size);
    p.set_fch_crc_ok(m.fch_crc_ok);
    p.set_pb_crc_ok(m.pb_crc_ok);
    p.set_pb_index(m.pb_index);
    p.set_pb_head(m.pb_head);
    p.set_pb_heads(to_std(m.pb_heads));
    for (bool ok : m.pb_crc_oks) p.add_pb_crc_oks(ok);
    p.set_beacon_timestamp(m.beacon_timestamp);
    p.set_beacon_line(m.beacon_line);
    p.set_beacon_type(m.beacon_type);
    p.set_beacon_netsn(m.beacon_netsn);
    p.set_beacon_cco_mac(m.beacon_cco_mac);
    p.set_beacon_period_cnt(m.beacon_period_cnt);
    p.set_beacon_rf_channel(m.beacon_rf_channel);
    p.set_beacon_rf_option(m.beacon_rf_option);
    p.set_beacon_item_num(m.beacon_item_num);
    p.set_coord_duration(m.coord_duration);
    p.set_coord_shift(m.coord_shift);
    p.set_coord_neighbour_nid(m.coord_neighbour_nid);
    p.set_coord_rf_channel(m.coord_rf_channel);
    p.set_coord_rsv0(m.coord_rsv0);
    p.set_coord_rsv1(m.coord_rsv1);
    p.set_coord_rsv2(m.coord_rsv2);
    p.set_coord_rsv3(m.coord_rsv3);
    p.set_coord_band_end_flag(m.coord_band_end_flag);
    p.set_coord_option(m.coord_option);
    p.set_coord_band_end_offset(m.coord_band_end_offset);
    p.set_coord_band_start_offset(m.coord_band_start_offset);
    p.set_ack_ext_type(m.ack_ext_type);
    p.set_ack_rx_res(m.ack_rx_res);
    p.set_ack_rx_status(m.ack_rx_status);
    p.set_ack_rx_pb_num(m.ack_rx_pb_num);
    p.set_ack_rsv0(m.ack_rsv0);
    p.set_ack_channel_quality(m.ack_channel_quality);
    p.set_ack_sta_load(m.ack_sta_load);
    p.set_ack_rsv1(m.ack_rsv1);
    p.set_ack_dst_addr(m.ack_dst_addr);
    p.set_ack_search_tei(m.ack_search_tei);
    p.set_ack_search_freq(m.ack_search_freq);
    p.set_ack_sync_timestamp(m.ack_sync_timestamp);
    p.set_ack_sync_tei(m.ack_sync_tei);
    return p;
}
MpduInfo from_proto(const qtplugin::ipc::MpduInfo& p) {
    MpduInfo m;
    m.frame_type = static_cast<quint8>(p.frame_type());
    m.net_type = static_cast<quint8>(p.net_type());
    m.net_id = p.net_id();
    m.version = static_cast<quint8>(p.version());
    m.src_tei = static_cast<quint16>(p.src_tei());
    m.dst_tei = static_cast<quint16>(p.dst_tei());
    m.link_id = static_cast<quint8>(p.link_id());
    m.frame_len = static_cast<quint16>(p.frame_len());
    m.pb_num = static_cast<quint8>(p.pb_num());
    m.symbol_num = static_cast<quint16>(p.symbol_num());
    m.bc_flag = p.bc_flag();
    m.re_send_flag = p.re_send_flag();
    m.encryp_flag = p.encryp_flag();
    m.tmi = static_cast<quint8>(p.tmi());
    m.tmi_ext = static_cast<quint8>(p.tmi_ext());
    m.pb_size = static_cast<quint16>(p.pb_size());
    m.fch_crc_ok = p.fch_crc_ok();
    m.pb_crc_ok = p.pb_crc_ok();
    m.pb_index = p.pb_index();
    m.pb_head = static_cast<quint8>(p.pb_head());
    m.pb_heads = to_quint8_vec(p.pb_heads());
    for (int i = 0; i < p.pb_crc_oks_size(); ++i)
        m.pb_crc_oks.append(p.pb_crc_oks(i));
    m.beacon_timestamp = p.beacon_timestamp();
    m.beacon_line = static_cast<quint8>(p.beacon_line());
    m.beacon_type = static_cast<quint8>(p.beacon_type());
    m.beacon_netsn = static_cast<quint8>(p.beacon_netsn());
    m.beacon_cco_mac = p.beacon_cco_mac();
    m.beacon_period_cnt = p.beacon_period_cnt();
    m.beacon_rf_channel = static_cast<quint8>(p.beacon_rf_channel());
    m.beacon_rf_option = static_cast<quint8>(p.beacon_rf_option());
    m.beacon_item_num = p.beacon_item_num();
    m.coord_duration = static_cast<quint16>(p.coord_duration());
    m.coord_shift = static_cast<quint16>(p.coord_shift());
    m.coord_neighbour_nid = p.coord_neighbour_nid();
    m.coord_rf_channel = static_cast<quint8>(p.coord_rf_channel());
    m.coord_rsv0 = static_cast<quint8>(p.coord_rsv0());
    m.coord_rsv1 = static_cast<quint16>(p.coord_rsv1());
    m.coord_rsv2 = static_cast<quint8>(p.coord_rsv2());
    m.coord_rsv3 = static_cast<quint8>(p.coord_rsv3());
    m.coord_band_end_flag = static_cast<quint8>(p.coord_band_end_flag());
    m.coord_option = static_cast<quint8>(p.coord_option());
    m.coord_band_end_offset = static_cast<quint16>(p.coord_band_end_offset());
    m.coord_band_start_offset = static_cast<quint16>(p.coord_band_start_offset());
    m.ack_ext_type = static_cast<quint8>(p.ack_ext_type());
    m.ack_rx_res = static_cast<quint8>(p.ack_rx_res());
    m.ack_rx_status = static_cast<quint8>(p.ack_rx_status());
    m.ack_rx_pb_num = static_cast<quint8>(p.ack_rx_pb_num());
    m.ack_rsv0 = static_cast<quint8>(p.ack_rsv0());
    m.ack_channel_quality = static_cast<quint8>(p.ack_channel_quality());
    m.ack_sta_load = static_cast<quint8>(p.ack_sta_load());
    m.ack_rsv1 = static_cast<quint8>(p.ack_rsv1());
    m.ack_dst_addr = p.ack_dst_addr();
    m.ack_search_tei = static_cast<quint16>(p.ack_search_tei());
    m.ack_search_freq = static_cast<quint8>(p.ack_search_freq());
    m.ack_sync_timestamp = p.ack_sync_timestamp();
    m.ack_sync_tei = static_cast<quint16>(p.ack_sync_tei());
    return m;
}

// ---- MsduFieldNode (递归) ----
qtplugin::ipc::MsduFieldNode to_proto(const MsduFieldNode& n) {
    qtplugin::ipc::MsduFieldNode p;
    p.set_name(n.name.toStdString());
    p.set_value(n.value.toStdString());
    for (const MsduFieldNode& c : n.children)
        *p.add_children() = to_proto(c);
    p.set_rel_start(n.rel_start);
    p.set_rel_len(n.rel_len);
    return p;
}
MsduFieldNode from_proto(const qtplugin::ipc::MsduFieldNode& p) {
    MsduFieldNode n;
    n.name = QString::fromStdString(p.name());
    n.value = QString::fromStdString(p.value());
    for (int i = 0; i < p.children_size(); ++i)
        n.children.append(from_proto(p.children(i)));
    n.rel_start = p.rel_start();
    n.rel_len = p.rel_len();
    return n;
}

// ---- TeiMacPair ----
qtplugin::ipc::TeiMacPair to_proto(const TeiMacPair& p) {
    qtplugin::ipc::TeiMacPair m;
    m.set_tei(p.tei);
    m.set_mac(p.mac);
    return m;
}
TeiMacPair from_proto(const qtplugin::ipc::TeiMacPair& p) {
    TeiMacPair r;
    r.tei = static_cast<quint16>(p.tei());
    r.mac = p.mac();
    return r;
}

// ---- CommRateInfo ----
qtplugin::ipc::CommRateInfo to_proto(const CommRateInfo& c) {
    qtplugin::ipc::CommRateInfo m;
    m.set_tei(c.tei);
    m.set_down(c.down);
    m.set_up(c.up);
    return m;
}
CommRateInfo from_proto(const qtplugin::ipc::CommRateInfo& p) {
    CommRateInfo c;
    c.tei = static_cast<quint16>(p.tei());
    c.down = static_cast<quint8>(p.down());
    c.up = static_cast<quint8>(p.up());
    return c;
}

// ---- TopoEvent ----
qtplugin::ipc::TopoEvent to_proto(const TopoEvent& e) {
    qtplugin::ipc::TopoEvent p;
    p.set_kind(static_cast<qtplugin::ipc::TopoEventKind>(e.kind));
    p.set_nid(e.nid);
    p.set_cco_mac(e.cco_mac);
    for (const TeiMacPair& n : e.nodes) *p.add_nodes() = to_proto(n);
    for (const QPair<quint16, quint16>& r : e.routes) {
        qtplugin::ipc::RoutePair* rp = p.add_routes();
        rp->set_first(r.first);
        rp->set_second(r.second);
    }
    for (const QPair<quint16, quint16>& r : e.up_routes) {
        qtplugin::ipc::RoutePair* rp = p.add_up_routes();
        rp->set_first(r.first);
        rp->set_second(r.second);
    }
    p.set_discover_src_tei(e.discover_src_tei);
    for (quint16 t : e.neighbor_teis) p.add_neighbor_teis(t);
    for (quint64 mac : e.leaves) p.add_leaves(mac);
    for (const CommRateInfo& c : e.comm_rates) *p.add_comm_rates() = to_proto(c);
    p.set_is_rf(e.is_rf);
    p.set_restart_count(e.restart_count);
    p.set_desc(e.desc.toStdString());
    p.set_epoch_ms(e.epoch_ms);
    p.set_frame_index(e.frame_index);
    return p;
}
TopoEvent from_proto(const qtplugin::ipc::TopoEvent& p) {
    TopoEvent e;
    e.kind = static_cast<TopoEventKind>(p.kind());
    e.nid = p.nid();
    e.cco_mac = p.cco_mac();
    for (int i = 0; i < p.nodes_size(); ++i)
        e.nodes.append(from_proto(p.nodes(i)));
    for (int i = 0; i < p.routes_size(); ++i)
        e.routes.append(qMakePair(static_cast<quint16>(p.routes(i).first()),
                                  static_cast<quint16>(p.routes(i).second())));
    for (int i = 0; i < p.up_routes_size(); ++i)
        e.up_routes.append(qMakePair(static_cast<quint16>(p.up_routes(i).first()),
                                     static_cast<quint16>(p.up_routes(i).second())));
    e.discover_src_tei = static_cast<quint16>(p.discover_src_tei());
    for (int i = 0; i < p.neighbor_teis_size(); ++i)
        e.neighbor_teis.append(static_cast<quint16>(p.neighbor_teis(i)));
    for (int i = 0; i < p.leaves_size(); ++i)
        e.leaves.append(p.leaves(i));
    for (int i = 0; i < p.comm_rates_size(); ++i)
        e.comm_rates.append(from_proto(p.comm_rates(i)));
    e.is_rf = p.is_rf();
    e.restart_count = p.restart_count();
    e.desc = QString::fromStdString(p.desc());
    e.epoch_ms = p.epoch_ms();
    e.frame_index = p.frame_index();
    return e;
}

// ---- MsduInfo ----
qtplugin::ipc::MsduInfo to_proto(const MsduInfo& m) {
    qtplugin::ipc::MsduInfo p;
    p.set_present(m.present);
    p.set_simple_head(m.simple_head);
    p.set_msdu_seq(m.msdu_seq);
    p.set_msdu_src_tei(m.msdu_src_tei);
    p.set_msdu_dst_tei(m.msdu_dst_tei);
    p.set_msdu_send_type(m.msdu_send_type);
    p.set_msdu_src_mac(m.msdu_src_mac);
    p.set_msdu_dst_mac(m.msdu_dst_mac);
    p.set_sta_mac(m.sta_mac);
    p.set_vlan_tag(m.vlan_tag);
    p.set_msdu_type(m.msdu_type);
    p.set_restart_count(m.restart_count);
    p.set_broadcast_direction(m.broadcast_direction);
    p.set_business_id(m.business_id);
    p.set_app_packet_type(m.app_packet_type);
    p.set_mme_type(m.mme_type);
    p.set_total_len(m.total_len);
    p.set_summary(m.summary.toStdString());
    for (const MsduFieldNode& n : m.tree) *p.add_tree() = to_proto(n);
    for (const TeiMacPair& t : m.tei_mac_pairs) *p.add_tei_mac_pairs() = to_proto(t);
    *p.mutable_topo_event() = to_proto(m.topo_event);
    return p;
}
MsduInfo from_proto(const qtplugin::ipc::MsduInfo& p) {
    MsduInfo m;
    m.present = p.present();
    m.simple_head = p.simple_head();
    m.msdu_seq = static_cast<quint16>(p.msdu_seq());
    m.msdu_src_tei = p.msdu_src_tei();
    m.msdu_dst_tei = p.msdu_dst_tei();
    m.msdu_send_type = p.msdu_send_type();
    m.msdu_src_mac = p.msdu_src_mac();
    m.msdu_dst_mac = p.msdu_dst_mac();
    m.sta_mac = p.sta_mac();
    m.vlan_tag = p.vlan_tag();
    m.msdu_type = static_cast<quint16>(p.msdu_type());
    m.restart_count = static_cast<quint8>(p.restart_count());
    m.broadcast_direction = static_cast<quint8>(p.broadcast_direction());
    m.business_id = static_cast<quint8>(p.business_id());
    m.app_packet_type = static_cast<quint8>(p.app_packet_type());
    m.mme_type = static_cast<quint16>(p.mme_type());
    m.total_len = p.total_len();
    m.summary = QString::fromStdString(p.summary());
    for (int i = 0; i < p.tree_size(); ++i)
        m.tree.append(from_proto(p.tree(i)));
    for (int i = 0; i < p.tei_mac_pairs_size(); ++i)
        m.tei_mac_pairs.append(from_proto(p.tei_mac_pairs(i)));
    m.topo_event = from_proto(p.topo_event());
    return m;
}

// ---- ParseResult ----
qtplugin::ipc::ParseResult to_proto(const ParseResult& r) {
    qtplugin::ipc::ParseResult p;
    *p.mutable_meta() = to_proto(r.meta);
    *p.mutable_mpdu() = to_proto(r.mpdu);
    p.set_msdu_body(to_std(r.msdu_body));
    *p.mutable_msdu() = to_proto(r.msdu);
    p.set_msdu_raw_base(r.msdu_raw_base);
    *p.mutable_beacon() = to_proto(r.beacon);
    p.set_arrival_us(r.arrival_us);
    p.set_raw_wire(to_std(r.raw_wire));
    p.set_accept(r.accept);
    p.set_reject_reason(r.reject_reason.toStdString());
    p.set_payload_for_log(to_std(r.payload_for_log));
    return p;
}
ParseResult from_proto(const qtplugin::ipc::ParseResult& p) {
    ParseResult r;
    r.meta = from_proto(p.meta());
    r.mpdu = from_proto(p.mpdu());
    r.msdu_body = to_qba(p.msdu_body());
    r.msdu = from_proto(p.msdu());
    r.msdu_raw_base = p.msdu_raw_base();
    r.beacon = from_proto(p.beacon());
    r.arrival_us = p.arrival_us();
    r.raw_wire = to_qba(p.raw_wire());
    r.accept = p.accept();
    r.reject_reason = QString::fromStdString(p.reject_reason());
    r.payload_for_log = to_qba(p.payload_for_log());
    return r;
}

// ---- GraphicsEvent (Phase3) ----
qtplugin::ipc::GraphicsEvent to_proto(const GraphicsEvent& e) {
    qtplugin::ipc::GraphicsEvent p;
    p.set_type(static_cast<qtplugin::ipc::GraphicsEventType>(e.type));
    p.set_x(e.x);
    p.set_y(e.y);
    p.set_button(e.button);
    p.set_modifiers(e.modifiers);
    p.set_delta_y(e.delta_y);
    p.set_width(e.width);
    p.set_height(e.height);
    return p;
}
GraphicsEvent from_proto(const qtplugin::ipc::GraphicsEvent& p) {
    GraphicsEvent e;
    e.type = static_cast<GraphicsEventType>(p.type());
    e.x = p.x();
    e.y = p.y();
    e.button = p.button();
    e.modifiers = p.modifiers();
    e.delta_y = p.delta_y();
    e.width = p.width();
    e.height = p.height();
    return e;
}

} // namespace plugin_proto
