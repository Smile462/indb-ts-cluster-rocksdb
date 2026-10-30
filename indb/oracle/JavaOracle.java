//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Reference oracle for the numerical kernel.
//
// Loads the K-Shape and Medoid-Shape classes that ship with the IoTDB
// implementation and runs them on a flat series, producing the same k x l
// centre matrix the C++ kernel produces for its from-scratch paths. Compiling
// and running against the deployed library-udf jar makes the original Java
// implementation the ground truth, rather than comparing the two new ports
// against each other.
//
// Usage: JavaOracle <k|m> <series.csv> <l> <k> <edk> <r> [iterations]

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

import org.apache.iotdb.library.dmatch.util.KShape;
import org.apache.iotdb.library.dmatch.util.KShapeM;

public final class JavaOracle {

  public static void main(String[] args) throws Exception {
    if (args.length < 6) {
      System.err.println("usage: JavaOracle <k|m> <series.csv> <l> <k> <edk> <r> [iterations]");
      System.exit(2);
    }
    final boolean medoid = args[0].equalsIgnoreCase("m");
    final String path = args[1];
    final int l = Integer.parseInt(args[2]);
    final int k = Integer.parseInt(args[3]);
    final int edk = Integer.parseInt(args[4]);
    final double r = Double.parseDouble(args[5]);
    final int iterations = args.length > 6 ? Integer.parseInt(args[6]) : 100;

    final List<Double> values = new ArrayList<>();
    try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
      String line;
      while ((line = reader.readLine()) != null) {
        line = line.trim();
        if (line.isEmpty()) {
          continue;
        }
        final int comma = line.indexOf(',');
        final String field = comma >= 0 ? line.substring(comma + 1) : line;
        try {
          values.add(Double.parseDouble(field.trim()));
        } catch (NumberFormatException ignored) {
          // Header or comment line.
        }
      }
    }

    final int count = values.size() / l;
    if (count < k) {
      System.err.println("not enough complete subsequences: " + count);
      System.exit(3);
    }

    final double[][] x = new double[count][l];
    for (int i = 0; i < count; i++) {
      for (int j = 0; j < l; j++) {
        x[i][j] = values.get(i * l + j);
      }
    }

    final double[][] centers;
    if (medoid) {
      final KShapeM model = new KShapeM(k, edk, r, iterations);
      centers = model.fit(x);
    } else {
      final KShape model = new KShape(k, iterations);
      centers = model.fit(x);
    }

    final StringBuilder out = new StringBuilder();
    out.append(centers.length).append(' ').append(centers.length == 0 ? 0 : centers[0].length)
        .append('\n');
    for (double[] row : centers) {
      for (int j = 0; j < row.length; j++) {
        out.append(String.format("%.17g", row[j]));
        out.append(j + 1 == row.length ? '\n' : ' ');
      }
    }
    System.out.print(out);
  }
}
