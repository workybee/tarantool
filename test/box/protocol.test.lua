box.schema.user.grant('guest', 'read,write,execute', 'universe')

--------------------------------------------------------------------------------
-- Test case for #273: IPROTO_ITERATOR ignored in network protocol
--------------------------------------------------------------------------------

space = box.schema.create_space('tweedledum')
space:create_index('primary', { type = 'tree'})
for i=1,5 do space:insert{i} end

port = string.gsub(box.cfg.listen, '^.*:', '')
conn = (require 'net.box'):new('127.0.0.1', tonumber(port))
conn.space[space.id]:select(3, { iterator = 'GE' })
conn.space[space.id]:select(3, { iterator = 'LE' })
conn.space[space.id]:select(3, { iterator = 'GT' })
conn.space[space.id]:select(3, { iterator = 'LT' })
conn:close()

space:drop()