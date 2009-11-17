/*
 * Bittorrent Client using Qt4 and libtorrent.
 * Copyright (C) 2006  Christophe Dumez
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 *
 * Contact : chris@qbittorrent.org
 */

#include "peerlistwidget.h"
#include "peerlistdelegate.h"
#include "reverseresolution.h"
#include "preferences.h"
#include "propertieswidget.h"
#include "geoip.h"
#include "peeraddition.h"
#include "speedlimitdlg.h"
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QSettings>
#include <QMenu>
#include <vector>

PeerListWidget::PeerListWidget(PropertiesWidget *parent): properties(parent), display_flags(false) {
  // Visual settings
  setRootIsDecorated(false);
  setItemsExpandable(false);
  setAllColumnsShowFocus(true);
  // List Model
  listModel = new QStandardItemModel(0, 7);
  listModel->setHeaderData(IP, Qt::Horizontal, tr("IP"));
  listModel->setHeaderData(CLIENT, Qt::Horizontal, tr("Client", "i.e.: Client application"));
  listModel->setHeaderData(PROGRESS, Qt::Horizontal, tr("Progress", "i.e: % downloaded"));
  listModel->setHeaderData(DOWN_SPEED, Qt::Horizontal, tr("Down Speed", "i.e: Download speed"));
  listModel->setHeaderData(UP_SPEED, Qt::Horizontal, tr("Up Speed", "i.e: Upload speed"));
  listModel->setHeaderData(TOT_DOWN, Qt::Horizontal, tr("Downloaded", "i.e: total data downloaded"));
  listModel->setHeaderData(TOT_UP, Qt::Horizontal, tr("Uploaded", "i.e: total data uploaded"));
  // Proxy model to support sorting without actually altering the underlying model
  proxyModel = new QSortFilterProxyModel();
  proxyModel->setDynamicSortFilter(true);
  proxyModel->setSourceModel(listModel);
  setModel(proxyModel);
  // Context menu
  setContextMenuPolicy(Qt::CustomContextMenu);
  connect(this, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(showPeerListMenu(QPoint)));
  // List delegate
  listDelegate = new PeerListDelegate(this);
  setItemDelegate(listDelegate);
  // Enable sorting
  setSortingEnabled(true);
  // Load settings
  loadSettings();
  // IP to Hostname resolver
  updatePeerHostNameResolutionState();
}

PeerListWidget::~PeerListWidget() {
  saveSettings();
  delete proxyModel;
  delete listModel;
  delete listDelegate;
  if(resolver)
    delete resolver;
}

void PeerListWidget::updatePeerHostNameResolutionState() {
  if(Preferences::resolvePeerHostNames()) {
    if(!resolver) {
      resolver = new ReverseResolution(this);
      connect(resolver, SIGNAL(ip_resolved(QString,QString)), this, SLOT(handleResolved(QString,QString)));
      resolver->start();
      loadPeers(properties->getCurrentTorrent(), true);
    }
  } else {
    if(resolver)
      resolver->asyncDelete();
  }
}

void PeerListWidget::updatePeerCountryResolutionState() {
  if(Preferences::resolvePeerCountries() != display_flags) {
    display_flags = !display_flags;
    if(display_flags) {
      const QTorrentHandle &h = properties->getCurrentTorrent();
      if(!h.is_valid()) return;
      loadPeers(h);
    }
  }
}

void PeerListWidget::showPeerListMenu(QPoint) {
  QMenu menu;
  QTorrentHandle h = properties->getCurrentTorrent();
  if(!h.is_valid()) return;
  QModelIndexList selectedIndexes = selectionModel()->selectedRows();
  QStringList selectedPeerIPs;
  foreach(const QModelIndex &index, selectedIndexes) {
    QString IP = proxyModel->data(index).toString();
    selectedPeerIPs << IP;
  }
  // Add Peer Action
  QAction *addPeerAct = 0;
  if(!h.is_queued() && !h.is_checking()) {
    addPeerAct = menu.addAction(QIcon(":/Icons/oxygen/add_peer.png"), tr("Add a new peer"));
  }
  // Per Peer Speed limiting actions
  QAction *upLimitAct = 0;
  QAction *dlLimitAct = 0;
  if(!selectedPeerIPs.isEmpty()) {
    upLimitAct = menu.addAction(QIcon(":/Icons/skin/seeding.png"), tr("Limit upload rate"));
    dlLimitAct = menu.addAction(QIcon(":/Icons/skin/downloading.png"), tr("Limit download rate"));
  }
  QAction *act = menu.exec(QCursor::pos());
  if(act == addPeerAct) {
    boost::asio::ip::tcp::endpoint ep = PeerAdditionDlg::askForPeerEndpoint();
    if(ep != boost::asio::ip::tcp::endpoint()) {
      try {
        h.connect_peer(ep);
        QMessageBox::information(0, tr("Peer addition"), tr("The peer was added to this torrent."));
      } catch(std::exception) {
        QMessageBox::critical(0, tr("Peer addition"), tr("The peer could not be added to this torrent."));
      }
    } else {
      qDebug("No peer was added");
    }
    return;
  }
  if(act == upLimitAct) {
    limitUpRateSelectedPeers(selectedPeerIPs);
    return;
  }
  if(act == dlLimitAct) {
    limitDlRateSelectedPeers(selectedPeerIPs);
    return;
  }
}

void PeerListWidget::limitUpRateSelectedPeers(QStringList peer_ips) {
  QTorrentHandle h = properties->getCurrentTorrent();
  if(!h.is_valid()) return;
  bool ok=false;
  long limit = SpeedLimitDialog::askSpeedLimit(&ok, tr("Upload rate limiting"), -1);
  if(!ok) return;
  foreach(const QString &ip, peer_ips) {
    boost::asio::ip::tcp::endpoint ep = peerEndpoints.value(ip, boost::asio::ip::tcp::endpoint());
    if(ep != boost::asio::ip::tcp::endpoint()) {
      qDebug("Settings Upload limit of %.1f Kb/s to peer %s", limit/1024., ip.toLocal8Bit().data());
      try {
        h.set_peer_upload_limit(ep, limit);
      }catch(std::exception) {
        std::cerr << "Impossible to apply upload limit to peer" << std::endl;
      }
    } else {
      qDebug("The selected peer no longer exists...");
    }
  }
}

void PeerListWidget::limitDlRateSelectedPeers(QStringList peer_ips) {
  QTorrentHandle h = properties->getCurrentTorrent();
  if(!h.is_valid()) return;
  bool ok=false;
  long limit = SpeedLimitDialog::askSpeedLimit(&ok, tr("Download rate limiting"), -1);
  if(!ok) return;
  foreach(const QString &ip, peer_ips) {
    boost::asio::ip::tcp::endpoint ep = peerEndpoints.value(ip, boost::asio::ip::tcp::endpoint());
    if(ep != boost::asio::ip::tcp::endpoint()) {
      qDebug("Settings Download limit of %.1f Kb/s to peer %s", limit/1024., ip.toLocal8Bit().data());
      try {
        h.set_peer_download_limit(ep, limit);
      }catch(std::exception) {
        std::cerr << "Impossible to apply download limit to peer" << std::endl;
      }
    } else {
      qDebug("The selected peer no longer exists...");
    }
  }
}


void PeerListWidget::clear() {
  qDebug("clearing peer list");
  peerItems.clear();
  peerEndpoints.clear();
  missingFlags.clear();
  int nbrows = listModel->rowCount();
  if(nbrows > 0) {
    qDebug("Cleared %d peers", nbrows);
    listModel->removeRows(0,  nbrows);
  }
}

void PeerListWidget::loadSettings() {
  QSettings settings(QString::fromUtf8("qBittorrent"), QString::fromUtf8("qBittorrent"));
  QVariantList contentColsWidths = settings.value(QString::fromUtf8("TorrentProperties/Peers/peersColsWidth"), QVariantList()).toList();
  if(!contentColsWidths.empty()) {
    for(int i=0; i<contentColsWidths.size(); ++i) {
      setColumnWidth(i, contentColsWidths.at(i).toInt());
    }
  }
}

void PeerListWidget::saveSettings() const {
  QSettings settings(QString::fromUtf8("qBittorrent"), QString::fromUtf8("qBittorrent"));
  QVariantList contentColsWidths;
  for(int i=0; i<listModel->columnCount(); ++i) {
    contentColsWidths.append(columnWidth(i));
  }
  settings.setValue(QString::fromUtf8("TorrentProperties/Peers/peersColsWidth"), contentColsWidths);
}

void PeerListWidget::loadPeers(const QTorrentHandle &h, bool force_hostname_resolution) {
  if(!h.is_valid()) return;
  std::vector<peer_info> peers;
  h.get_peer_info(peers);
  std::vector<peer_info>::iterator itr;
  QSet<QString> old_peers_set = peerItems.keys().toSet();
  for(itr = peers.begin(); itr != peers.end(); itr++) {
    peer_info peer = *itr;
    QString peer_ip = misc::toQString(peer.ip.address().to_string());
    if(peerItems.contains(peer_ip)) {
      // Update existing peer
      updatePeer(peer_ip, peer);
      old_peers_set.remove(peer_ip);
      if(force_hostname_resolution) {
        if(resolver)
          resolver->resolve(peer.ip);
      }
    } else {
      // Add new peer
      peerItems[peer_ip] = addPeer(peer_ip, peer);
      peerEndpoints[peer_ip] = peer.ip;
    }
  }
  // Delete peers that are gone
  QSetIterator<QString> it(old_peers_set);
  while(it.hasNext()) {
    QString ip = it.next();
    missingFlags.remove(ip);
    peerEndpoints.remove(ip);
    QStandardItem *item = peerItems.take(ip);
    listModel->removeRow(item->row());
  }
}

QStandardItem* PeerListWidget::addPeer(QString ip, peer_info peer) {
  int row = listModel->rowCount();
  // Adding Peer to peer list
  listModel->insertRow(row);
  listModel->setData(listModel->index(row, IP), ip);
  // Resolve peer host name is asked
  if(resolver)
    resolver->resolve(peer.ip);
  if(display_flags) {
    QIcon ico = GeoIP::CountryISOCodeToIcon(peer.country);
    if(!ico.isNull()) {
      listModel->setData(listModel->index(row, IP), ico, Qt::DecorationRole);
    } else {
      missingFlags.insert(ip);
    }
  }
  listModel->setData(listModel->index(row, CLIENT), misc::toQString(peer.client));
  listModel->setData(listModel->index(row, PROGRESS), peer.progress);
  listModel->setData(listModel->index(row, DOWN_SPEED), peer.payload_down_speed);
  listModel->setData(listModel->index(row, UP_SPEED), peer.payload_up_speed);
  listModel->setData(listModel->index(row, TOT_DOWN), peer.total_download);
  listModel->setData(listModel->index(row, TOT_UP), peer.total_upload);
  return listModel->item(row, IP);
}

void PeerListWidget::updatePeer(QString ip, peer_info peer) {
  QStandardItem *item = peerItems.value(ip);
  int row = item->row();
  if(display_flags) {
    QIcon ico = GeoIP::CountryISOCodeToIcon(peer.country);
    if(!ico.isNull()) {
      listModel->setData(listModel->index(row, IP), ico, Qt::DecorationRole);
      missingFlags.remove(ip);
    }
  }
  listModel->setData(listModel->index(row, CLIENT), misc::toQString(peer.client));
  listModel->setData(listModel->index(row, PROGRESS), peer.progress);
  listModel->setData(listModel->index(row, DOWN_SPEED), peer.payload_down_speed);
  listModel->setData(listModel->index(row, UP_SPEED), peer.payload_up_speed);
  listModel->setData(listModel->index(row, TOT_DOWN), peer.total_download);
  listModel->setData(listModel->index(row, TOT_UP), peer.total_upload);
}

void PeerListWidget::handleResolved(QString ip, QString hostname) {
  QStandardItem *item = peerItems.value(ip, 0);
  if(item) {
    listModel->setData(listModel->indexFromItem(item), hostname);
  }
}
