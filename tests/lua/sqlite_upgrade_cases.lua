-- Shared actual receiver behavior for ordinary/packed apps and default workers.
return function(db, null)
  local function scalar(sql)
    local cursor <close> = assert(db:query(sql,{}))
    local row=assert(cursor:next())
    assert(cursor:next()==nil)
    return row[1]
  end
  assert(scalar('SELECT sqlite_version()')=='3.54.1')
  assert(scalar('SELECT '..string.rep('1+',999)..'1')==1000)
  local too_deep='SELECT '..string.rep('1+',1000)..'1'
  local cursor,err=db:query(too_deep,{})
  assert(cursor==nil and err.dependency_code==1 and err.message:find('Expression tree',1,true))
  local ok,failure=db:transaction(function(connection)
    local value,problem=connection:query(too_deep,{})
    assert(value==nil and problem.dependency_code==1)
    return nil,problem
  end)
  assert(ok==nil and failure.transaction_outcome=='rolled_back')
  assert(scalar('SELECT 42')==42)
  assert(db:exec('CREATE TABLE upgrade_operand(x)',{}))
  assert(db:exec('INSERT INTO upgrade_operand VALUES(1),(-3),(0),(5),(NULL)',{}))
  assert(db:exec('CREATE INDEX upgrade_expression ON upgrade_operand(x+2)',{}))
  for _,access in ipairs({'INDEXED BY upgrade_expression','NOT INDEXED'}) do
    assert(scalar('SELECT sum((x+2)*(x+3)) FROM upgrade_operand '..access)==74)
    assert(scalar('SELECT (x+2)*((x+3)+(x+4)) FROM upgrade_operand '..access..' WHERE x=1')==27)
  end
  local values={'1','0','NULL'}
  local and_values={{1,0,null},{0,0,0},{null,0,null}}
  local or_values={{1,1,1},{1,0,null},{1,null,null}}
  for i,a in ipairs(values) do
    for j,b in ipairs(values) do
      assert(scalar('SELECT '..a..' AND '..b)==and_values[i][j])
      assert(scalar('SELECT '..a..' OR '..b)==or_values[i][j])
    end
  end
  for _,depth in ipairs({8,400}) do
    local sql='SELECT '..string.rep('abs(',depth)..'1'..string.rep(')',depth)
    local prepared,problem=db:query(sql,{})
    if prepared then
      local row=assert(prepared:next());assert(row[1]==1)
      assert(prepared:next()==nil);assert(prepared:close())
    else
      assert(depth==400 and problem.dependency_code==1 and
        (problem.message:find('Recursion limit',1,true) or
         problem.message:find('Expression tree',1,true) or
         problem.message:find('native stack budget exhausted',1,true)))
    end
    assert(scalar('SELECT 42')==42)
  end
  -- New SQL behavior flows through the existing API and authorizer.
  assert(scalar([[SELECT format('%J','a"b')]])=='"a\\"b"')
  assert(scalar([[SELECT date('2026-11-10','end of month')]])=='2026-11-30')
  assert(db:exec([[CREATE TABLE upgrade_defaults(value TEXT DEFAULT(lower('VALUE')))]],{}))
  assert(db:exec('INSERT INTO upgrade_defaults DEFAULT VALUES',{}))
  assert(scalar('SELECT value FROM upgrade_defaults')=='value')
  local changed,change_error=db:exec('ALTER TABLE upgrade_defaults ADD COLUMN rowid INTEGER',{})
  assert(changed==nil and change_error.dependency_code==1)
  assert(scalar('SELECT 42')==42)
  assert(db:exec('DROP TABLE upgrade_defaults',{}))
  assert(db:exec('DROP TABLE upgrade_operand',{}))
end
