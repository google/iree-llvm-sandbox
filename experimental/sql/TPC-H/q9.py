import ibis


def get_ibis_query(COLOR="green"):
  from tpc_h_tables import part, supplier, lineitem, partsupp, orders, nation

  q = lineitem
  q = q.join(supplier, supplier.s_suppkey == lineitem.l_suppkey)
  q = q.join(
      partsupp,
      (partsupp.ps_suppkey == lineitem.l_suppkey) &
      (partsupp.ps_partkey == lineitem.l_partkey),
  )
  q = q.join(part, part.p_partkey == lineitem.l_partkey)
  q = q.join(orders, orders.o_orderkey == lineitem.l_orderkey)
  q = q.join(nation, supplier.s_nationkey == nation.n_nationkey)

  q = q[(q.l_extendedprice * (1 - q.l_discount) -
         q.ps_supplycost * q.l_quantity).name("amount"),
        q.o_orderdate.year().cast("string").name("o_year"),
        q.n_name.name("nation"), q.p_name,]

  q = q.filter([q.p_name.like("%" + COLOR + "%")])

  gq = q.group_by([q.nation, q.o_year])
  q = gq.aggregate(sum_profit=q.amount.sum())
  q = q.sort_by([q.nation, ibis.desc(q.o_year)])
  return q
