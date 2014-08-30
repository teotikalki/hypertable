/*
 * Copyright (C) 2007-2014 Hypertable, Inc.
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/** @file
 * Declaration for MetricsProcess.
 * This file contains the class definition of MetricsProcess, a class for
 * computing process metrics.
 */

package org.hypertable.Common;

import java.lang.Long;
import java.lang.Runtime;
import java.lang.System;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.util.HashMap;

import org.hyperic.sigar.jmx.SigarProcess;
import org.hyperic.sigar.Mem;
import org.hyperic.sigar.Sigar;
import org.hyperic.sigar.SigarException;

/** Collects process metrics.
 */
public class MetricsProcess {

  public MetricsProcess() {
    SigarProcess process = new SigarProcess(mSigar);
    mLastSys = process.getTimeSys();
    mLastUser = process.getTimeUser();
    mLastTimestamp = System.currentTimeMillis();
  }

  public void collect(long now, MetricsCollector collector) {
    HashMap<GarbageCollectorMXBean, Long> collectionCounts = new HashMap<GarbageCollectorMXBean, Long>();
    HashMap<GarbageCollectorMXBean, Long> collectionTimes = new HashMap<GarbageCollectorMXBean, Long>();
    long totalGarbageCollections = 0;
    long garbageCollectionTime = 0;

    for(GarbageCollectorMXBean gc :
          ManagementFactory.getGarbageCollectorMXBeans()) {

      long count = gc.getCollectionCount();
      collectionCounts.put(gc, count);
      if (mCollectionCounts.containsKey(gc)) {
        count -= mCollectionCounts.get(gc).longValue();
      }
      if (count >= 0) {
        totalGarbageCollections += count;
      }

      long time = gc.getCollectionTime();
      collectionTimes.put(gc, time);
      if (mCollectionTimes.containsKey(gc)) {
        time -= mCollectionTimes.get(gc).longValue();
      }
      if(time >= 0) {
        garbageCollectionTime += time;
      }
    }

    /* JVM GCs */
    collector.update("jvm.gc", (int)totalGarbageCollections);

    /* JVM GC time */
    collector.update("jvm.gcTime", (int)garbageCollectionTime);

    /* JVM heap size */
    double heapGb = (double)Runtime.getRuntime().totalMemory() / 1000000000.0;
    collector.update("jvm.heapSize", heapGb);

    SigarProcess process = new SigarProcess(mSigar);

    int pct;
    double gb;
    long cpuTimeSys = process.getTimeSys();
    long cpuTimeUser = process.getTimeUser();
    long elapsed_millis = (now - mLastTimestamp) / 1000000L;
    long diff_sys = (cpuTimeSys - mLastSys) / org.hypertable.Common.System.processorCount;
    long diff_user = (cpuTimeUser - mLastUser) / org.hypertable.Common.System.processorCount;

    if (elapsed_millis > 0) {
      /* CPU sys */
      pct = (int)((diff_sys * 100) / elapsed_millis);
      collector.update("cpu.sys", pct);

      /* CPU user */
      pct = (int)((diff_user * 100) / elapsed_millis);
      collector.update("cpu.user", pct);
    }

    /* Virtual memory */
    gb = (double)process.getMemVsize() / (double)(1024*1024*1024);
    collector.update("memory.virtual", gb);

    /* Resident memory */
    gb = (double)process.getMemResident() / (double)(1024*1024*1024);
    collector.update("memory.resident", gb);

    mCollectionCounts = collectionCounts;
    mCollectionTimes = collectionTimes;
    mLastTimestamp = now;
    mLastSys = cpuTimeSys;
    mLastUser = cpuTimeUser;
  }

  HashMap<GarbageCollectorMXBean, Long> mCollectionCounts = new HashMap<GarbageCollectorMXBean, Long>();
  HashMap<GarbageCollectorMXBean, Long> mCollectionTimes = new HashMap<GarbageCollectorMXBean, Long>();

  /** SIGAR */
  private static Sigar mSigar = new Sigar();

  /** Last collection timestamp in milliseconds */
  long mLastTimestamp = 0;

  /** Last recorded CPU system time */
  long mLastSys = 0;

  /** Last recorded CPU user time */
  long mLastUser = 0;

}
